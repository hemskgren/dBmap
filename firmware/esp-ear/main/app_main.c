#include <stdbool.h>
#include <string.h>

#include "dbmap_desired.h"
#include "dbmap_diag.h"
#include "dbmap_ear_mic.h"
#include "dbmap_ear_observation.h"
#include "dbmap_health.h"
#include "dbmap_mqtt.h"
#include "dbmap_nvs.h"
#include "dbmap_ota.h"
#include "dbmap_provision.h"
#include "dbmap_serial_config.h"
#include "dbmap_time.h"
#include "dbmap_types.h"
#include "dbmap_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "esp-ear";
static dbmap_effective_t s_cfg;
/** One-shot gate: Acoustic Monitor starts only after boot OTA handling. */
static bool s_acoustic_started;

static void start_acoustic_monitor_once(void)
{
    if (s_acoustic_started) {
        return;
    }
    s_acoustic_started = true;

    dbmap_ear_mic_config_t mic_cfg = {
        .model = DBMAP_EAR_MIC_MODEL_INMP441,
        .bclk_gpio = CONFIG_DBMAP_EAR_MIC_I2S_BCLK_GPIO,
        .ws_gpio = CONFIG_DBMAP_EAR_MIC_I2S_WS_GPIO,
        .din_gpio = CONFIG_DBMAP_EAR_MIC_I2S_DIN_GPIO,
        .mclk_gpio = CONFIG_DBMAP_EAR_MIC_I2S_MCLK_GPIO,
        .sample_rate_hz = CONFIG_DBMAP_EAR_MIC_SAMPLE_RATE_HZ,
    };
    esp_err_t mic_err = dbmap_ear_mic_start(&s_cfg, &mic_cfg);
    if (mic_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "microphone init failed (%s); continuing without audio capture",
            esp_err_to_name(mic_err)
        );
    }

    /* Observation publisher after mic so health enrichers chain (mic then observation). */
    esp_err_t obs_err = dbmap_ear_observation_start(&s_cfg);
    if (obs_err != ESP_OK) {
        ESP_LOGW(TAG, "observation publisher init failed (%s)", esp_err_to_name(obs_err));
    } else {
        ESP_LOGI(TAG, "Acoustic Monitor started (I2S + RMS + L0); independent of later MQTT/Wi-Fi drops");
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(dbmap_nvs_init());
    dbmap_diag_init();
    dbmap_effective_defaults(&s_cfg);
    ESP_ERROR_CHECK(dbmap_nvs_load(&s_cfg));

    ESP_LOGI(
        TAG,
        "DBMAP_DEVICE_ID=%s",
        s_cfg.identity.node_id[0] != '\0' ? s_cfg.identity.node_id : "UNPROVISIONED"
    );
    ESP_LOGI(
        TAG,
        "DBMAP_HUB_ID=%s",
        s_cfg.identity.hub_id[0] != '\0' ? s_cfg.identity.hub_id : "UNSET"
    );
    ESP_LOGI(
        TAG,
        "boot hardware=%s kind=%s mic=%s provisioned=%d",
        s_cfg.identity.hardware_revision,
        dbmap_mqtt_topic_kind(),
        dbmap_ear_mic_model_id(),
        (int)s_cfg.provisioned
    );
    ESP_LOGI(TAG, "device API credential %s", s_cfg.device_api_token[0] != '\0' ? "present" : "absent");

    if (s_cfg.provisioned) {
        ESP_LOGI(TAG, "USB configuration window open for 10 seconds");
        esp_err_t config_err = dbmap_serial_config_wait(&s_cfg, 10);
        if (config_err == ESP_OK) {
            ESP_LOGI(TAG, "USB configuration saved; restarting");
            esp_restart();
        }
        if (config_err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "USB configuration window ended: %s", esp_err_to_name(config_err));
        }
    }

    if (!s_cfg.provisioned && (s_cfg.network.ssid[0] == '\0' || s_cfg.bootstrap_token[0] == '\0')) {
        ESP_LOGI(
            TAG,
            "unprovisioned (missing Wi-Fi or bootstrap token); USB config window %d s then reboot",
            DBMAP_USB_BOOT_RECOVERY_S
        );
        (void)dbmap_serial_config_wait(&s_cfg, DBMAP_USB_BOOT_RECOVERY_S);
        esp_restart();
    }

    esp_err_t wifi_err = dbmap_wifi_start(&s_cfg.network);
    if (wifi_err != ESP_OK) {
        /* Keep retrying STA in the background; USB configure stays open.
         * Do not reboot solely because the configured SSID is absent. */
        (void)dbmap_boot_wait_wifi_ip_or_usb(&s_cfg, 0);
    }

    esp_err_t time_err = dbmap_time_sync(10000);
    if (time_err == ESP_OK) {
        ESP_LOGI(TAG, "system time synchronized with NTP");
    } else {
        ESP_LOGW(TAG, "system time not synchronized yet: %s", esp_err_to_name(time_err));
    }

    if (!s_cfg.provisioned) {
        esp_err_t perr = dbmap_hub_provision(&s_cfg);
        if (perr != ESP_OK) {
            ESP_LOGE(
                TAG,
                "secure provisioning failed; USB config window %d s then reboot",
                DBMAP_USB_BOOT_RECOVERY_S
            );
            (void)dbmap_serial_config_wait(&s_cfg, DBMAP_USB_BOOT_RECOVERY_S);
            esp_restart();
        }
    }

    /* Network + OTA path first; Acoustic Monitor waits until boot Desired is done. */
    ESP_ERROR_CHECK(dbmap_mqtt_start(&s_cfg, NULL, NULL));
    /*
     * Register OTA acoustic-close before any OTA-capable Desired path (including
     * synchronous boot Desired). Failure must not continue as if registered.
     */
    ESP_ERROR_CHECK(dbmap_ear_observation_register_ota_close());
    dbmap_ota_mark_valid_after_mqtt(&s_cfg);
    dbmap_desired_boot_check(&s_cfg);

    start_acoustic_monitor_once();

    /* Periodic Desired only — must not restart Acoustic Monitor on later polls. */
    dbmap_desired_poll_start(&s_cfg);

    int backoff_ms = 1000;
    while (1) {
        dbmap_ota_report_pending_boot(&s_cfg);
        if (!dbmap_mqtt_is_connected()) {
            dbmap_mqtt_request_reconnect();
        }
        const bool hello_ok = dbmap_publish_hello(&s_cfg) == ESP_OK;
        dbmap_publish_health(&s_cfg);
#if CONFIG_DBMAP_EAR_OBS_TEST_PUBLISH_ONCE && !CONFIG_DBMAP_EAR_OBS_RMS_ACTIVITY
        /* Phase 1 one-shot only when RMS activity task is not running. */
        dbmap_ear_observation_poll_test_trigger(&s_cfg);
#endif
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        /* Steady 30s hello/health when MQTT publish works. Back off only on failure.
         * (A previous reset-to-1s-on-success ran before delay and defeated the 30s cadence.) */
        if (hello_ok) {
            backoff_ms = 30000;
        } else if (backoff_ms < 120000) {
            backoff_ms *= 2;
        } else {
            backoff_ms = 120000;
        }
    }
}
