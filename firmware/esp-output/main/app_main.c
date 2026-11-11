#include <stdbool.h>
#include <string.h>

#include "dbmap_audio.h"
#include "sp_desired.h"
#include "sp_diag.h"
#include "sp_health.h"
#include "sp_mqtt.h"
#include "sp_nvs.h"
#include "sp_ota.h"
#include "sp_provision.h"
#include "sp_serial_config.h"
#include "sp_time.h"
#include "sp_types.h"
#include "sp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "output_command.h"

static const char *TAG = "esp-output";
static sp_effective_t s_cfg;

void app_main(void)
{
    ESP_ERROR_CHECK(sp_nvs_init());
    sp_diag_init();
    sp_effective_defaults(&s_cfg);
    ESP_ERROR_CHECK(sp_nvs_load(&s_cfg));

    ESP_LOGI(
        TAG,
        "node_id=%s",
        s_cfg.identity.node_id[0] != '\0' ? s_cfg.identity.node_id : "UNPROVISIONED"
    );
    ESP_LOGI(
        TAG,
        "hub_id=%s",
        s_cfg.identity.hub_id[0] != '\0' ? s_cfg.identity.hub_id : "UNSET"
    );
    ESP_LOGI(TAG, "boot hardware=%s provisioned=%d", s_cfg.identity.hardware_revision, (int)s_cfg.provisioned);
    ESP_LOGI(TAG, "device API credential %s", s_cfg.device_api_token[0] != '\0' ? "present" : "absent");

    if (s_cfg.provisioned) {
        ESP_LOGI(
            TAG,
            "USB configuration window open for %d seconds",
            SP_USB_PROVISIONED_CONFIG_WINDOW_S
        );
        esp_err_t config_err =
            sp_serial_config_wait(&s_cfg, SP_USB_PROVISIONED_CONFIG_WINDOW_S);
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
            SP_USB_BOOT_RECOVERY_S
        );
        (void)sp_serial_config_wait(&s_cfg, SP_USB_BOOT_RECOVERY_S);
        esp_restart();
    }

    esp_err_t wifi_err = sp_wifi_start(&s_cfg.network);
    if (wifi_err != ESP_OK) {
        /* Keep retrying STA in the background; USB configure stays open.
         * Do not reboot solely because the configured SSID is absent. */
        (void)sp_boot_wait_wifi_ip_or_usb(&s_cfg, 0);
    }

    esp_err_t time_err = sp_time_sync(10000);
    if (time_err == ESP_OK) {
        ESP_LOGI(TAG, "system time synchronized with NTP");
    } else {
        ESP_LOGW(TAG, "system time not synchronized yet: %s", esp_err_to_name(time_err));
    }

    if (!s_cfg.provisioned) {
        esp_err_t perr = sp_hub_provision(&s_cfg);
        if (perr != ESP_OK) {
            ESP_LOGE(
                TAG,
                "secure provisioning failed; USB config window %d s then reboot",
                SP_USB_BOOT_RECOVERY_S
            );
            (void)sp_serial_config_wait(&s_cfg, SP_USB_BOOT_RECOVERY_S);
            esp_restart();
        }
    }

    ESP_ERROR_CHECK(output_command_init());
    esp_err_t audio_err = sp_audio_init(s_cfg.network.hub_http_url);
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed (%s); continuing without playback", esp_err_to_name(audio_err));
    }
    ESP_ERROR_CHECK(sp_mqtt_start(&s_cfg, output_handle_command, &s_cfg));
    sp_ota_mark_valid_after_mqtt(&s_cfg);
    sp_desired_poll_start(&s_cfg);

    int backoff_ms = 1000;
    while (1) {
        sp_ota_report_pending_boot(&s_cfg);
        if (!sp_mqtt_is_connected()) {
            sp_mqtt_request_reconnect();
        }
        const bool hello_ok = sp_publish_hello(&s_cfg) == ESP_OK;
        sp_publish_health(&s_cfg);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        /* Steady 30s hello/health when MQTT publish works. Back off only on failure. */
        if (hello_ok) {
            backoff_ms = 30000;
        } else if (backoff_ms < 120000) {
            backoff_ms *= 2;
        } else {
            backoff_ms = 120000;
        }
    }
}
