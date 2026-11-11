#include <stdbool.h>
#include <string.h>

#include "sp_desired.h"
#include "sp_diag.h"
#include "dbmap_ear_acoustic.h"
#include "sp_mic.h"
#include "dbmap_ear_observation.h"
#include "sp_health.h"
#include "sp_mic_count.h"
#include "sp_mqtt.h"
#include "sp_nvs.h"
#include "sp_ota.h"
#include "sp_provision.h"
#include "sp_serial_config.h"
#include "sp_time.h"
#include "sp_types.h"
#include "sp_wifi.h"
#include "ear_boot_order.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG_PLATFORM = "platform_boot";
static const char *TAG_APP = "dbmap_app";
static sp_effective_t s_cfg;

/** Step 1 one-shots: mic before Wi-Fi; observation after Desired. */
static bool s_mic_start_attempted;
static bool s_obs_start_attempted;

static void start_ear_mic_once(void)
{
    dbmap_ear_boot_state_t st = {
        .mic_start_attempted = s_mic_start_attempted,
        .desired_boot_done = false,
        .obs_start_attempted = s_obs_start_attempted,
    };
    if (!dbmap_ear_boot_allow_mic_start(&st)) {
        return;
    }
    s_mic_start_attempted = true;

    /* Product acoustic hooks into Platform PCM retention before I2S starts. */
    (void)dbmap_ear_acoustic_register();

    sp_mic_config_t mic_cfg = {
        .model = SP_MIC_MODEL_INMP441,
        .bclk_gpio = CONFIG_DBMAP_EAR_MIC_I2S_BCLK_GPIO,
        .ws_gpio = CONFIG_DBMAP_EAR_MIC_I2S_WS_GPIO,
        .din_gpio = CONFIG_DBMAP_EAR_MIC_I2S_DIN_GPIO,
        .din1_gpio = CONFIG_DBMAP_EAR_MIC_I2S_DIN1_GPIO,
        .mclk_gpio = CONFIG_DBMAP_EAR_MIC_I2S_MCLK_GPIO,
        .sample_rate_hz = CONFIG_DBMAP_EAR_MIC_SAMPLE_RATE_HZ,
    };
    ESP_LOGI(TAG_PLATFORM, "starting Platform mic/I2S before Wi-Fi");
    esp_err_t mic_err = sp_mic_start(&s_cfg, &mic_cfg);
    if (mic_err != ESP_OK) {
        ESP_LOGW(
            TAG_PLATFORM,
            "microphone init failed (%s); continuing without audio capture",
            esp_err_to_name(mic_err)
        );
    } else {
        ESP_LOGI(
            TAG_PLATFORM,
            "microphone/I2S started before Wi-Fi; local capture independent of network"
        );
    }
}

static void start_ear_observation_once(void)
{
    dbmap_ear_boot_state_t st = {
        .mic_start_attempted = s_mic_start_attempted,
        .desired_boot_done = true, /* caller invokes only after Desired */
        .obs_start_attempted = s_obs_start_attempted,
    };
    if (!dbmap_ear_boot_allow_obs_start(&st)) {
        return;
    }
    s_obs_start_attempted = true;

    ESP_LOGI(TAG_APP, "starting dBmap Ear observation after Desired");
    esp_err_t obs_err = dbmap_ear_observation_start(&s_cfg);
    if (obs_err != ESP_OK) {
        ESP_LOGW(TAG_APP, "observation publisher init failed (%s)", esp_err_to_name(obs_err));
    } else {
        ESP_LOGI(
            TAG_APP,
            "Acoustic Monitor observation started (detector after Desired); "
            "independent of later MQTT/Wi-Fi drops"
        );
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(sp_nvs_init());
    sp_diag_init();
    sp_effective_defaults(&s_cfg);
    ESP_ERROR_CHECK(sp_nvs_load(&s_cfg));

    ESP_LOGI(
        TAG_PLATFORM,
        "node_id=%s",
        s_cfg.identity.node_id[0] != '\0' ? s_cfg.identity.node_id : "UNPROVISIONED"
    );
    ESP_LOGI(
        TAG_PLATFORM,
        "hub_id=%s",
        s_cfg.identity.hub_id[0] != '\0' ? s_cfg.identity.hub_id : "UNSET"
    );
    ESP_LOGI(
        TAG_PLATFORM,
        "boot hardware=%s kind=%s mic=%s provisioned=%d mic_count=%u (%s)",
        s_cfg.identity.hardware_revision,
        sp_mqtt_topic_kind(),
        sp_mic_model_id(),
        (int)s_cfg.provisioned,
        (unsigned)s_cfg.microphone_count,
        sp_mic_count_status_str(s_cfg.microphone_count_status)
    );
    ESP_LOGI(
        TAG_PLATFORM,
        "device API credential %s",
        s_cfg.device_api_token[0] != '\0' ? "present" : "absent"
    );

    if (s_cfg.provisioned) {
        ESP_LOGI(
            TAG_PLATFORM,
            "USB configuration window open for %d seconds",
            SP_USB_PROVISIONED_CONFIG_WINDOW_S
        );
        esp_err_t config_err =
            sp_serial_config_wait(&s_cfg, SP_USB_PROVISIONED_CONFIG_WINDOW_S);
        if (config_err == ESP_OK) {
            ESP_LOGI(TAG_PLATFORM, "USB configuration saved; restarting");
            esp_restart();
        }
        if (config_err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(
                TAG_PLATFORM,
                "USB configuration window ended: %s",
                esp_err_to_name(config_err)
            );
        }
    }

    if (!s_cfg.provisioned && (s_cfg.network.ssid[0] == '\0' || s_cfg.bootstrap_token[0] == '\0')) {
        ESP_LOGI(
            TAG_PLATFORM,
            "unprovisioned (missing Wi-Fi or bootstrap token); USB config window %d s then reboot",
            SP_USB_BOOT_RECOVERY_S
        );
        (void)sp_serial_config_wait(&s_cfg, SP_USB_BOOT_RECOVERY_S);
        esp_restart();
    }

    /* Platform mic/I2S before Wi-Fi so capture is not gated on connectivity. */
    start_ear_mic_once();

    ESP_LOGI(TAG_PLATFORM, "starting Wi-Fi after mic/I2S");
    esp_err_t wifi_err = sp_wifi_start(&s_cfg.network);
    if (wifi_err != ESP_OK) {
        /* Keep retrying STA in the background; USB configure stays open.
         * Do not reboot solely because the configured SSID is absent. */
        (void)sp_boot_wait_wifi_ip_or_usb(&s_cfg, 0);
    }

    esp_err_t time_err = sp_time_sync(10000);
    if (time_err == ESP_OK) {
        ESP_LOGI(TAG_PLATFORM, "system time synchronized with NTP");
    } else {
        ESP_LOGW(TAG_PLATFORM, "system time not synchronized yet: %s", esp_err_to_name(time_err));
    }

    if (!s_cfg.provisioned) {
        esp_err_t perr = sp_hub_provision(&s_cfg);
        if (perr != ESP_OK) {
            ESP_LOGE(
                TAG_PLATFORM,
                "secure provisioning failed; USB config window %d s then reboot",
                SP_USB_BOOT_RECOVERY_S
            );
            (void)sp_serial_config_wait(&s_cfg, SP_USB_BOOT_RECOVERY_S);
            esp_restart();
        }
    }

    ESP_ERROR_CHECK(sp_mqtt_start(&s_cfg, NULL, NULL));
    /*
     * Register OTA acoustic-close before any OTA-capable Desired path (including
     * synchronous boot Desired). Failure must not continue as if registered.
     * Close ends observation episodes only — it does not stop I2S/mic.
     */
    ESP_ERROR_CHECK(dbmap_ear_observation_register_ota_close());
    sp_ota_mark_valid_after_mqtt(&s_cfg);
    sp_desired_boot_check(&s_cfg);

    start_ear_observation_once();

    /* Periodic Desired only — must not restart mic or observation on later polls. */
    sp_desired_poll_start(&s_cfg);

    int backoff_ms = 1000;
    while (1) {
        sp_ota_report_pending_boot(&s_cfg);
        if (!sp_mqtt_is_connected()) {
            sp_mqtt_request_reconnect();
        }
        const bool hello_ok = sp_publish_hello(&s_cfg) == ESP_OK;
        sp_publish_health(&s_cfg);
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
