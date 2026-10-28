#include <string.h>

#include "dbmap_audio.h"
#include "dbmap_desired.h"
#include "dbmap_diag.h"
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
#include "output_command.h"

static const char *TAG = "esp-output";
static dbmap_effective_t s_cfg;

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
    ESP_LOGI(TAG, "boot hardware=%s provisioned=%d", s_cfg.identity.hardware_revision, (int)s_cfg.provisioned);
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
        ESP_ERROR_CHECK(dbmap_serial_config_wait(&s_cfg, 0));
        esp_restart();
    }

    esp_err_t wifi_err = dbmap_wifi_start(&s_cfg.network);
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi unavailable; waiting for corrected USB provisioning configuration");
        ESP_ERROR_CHECK(dbmap_serial_config_wait(&s_cfg, 0));
        esp_restart();
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
            ESP_LOGE(TAG, "secure provisioning failed; waiting for corrected USB configuration");
            ESP_ERROR_CHECK(dbmap_serial_config_wait(&s_cfg, 0));
            esp_restart();
        }
    }

    ESP_ERROR_CHECK(output_command_init());
    esp_err_t audio_err = dbmap_audio_init(s_cfg.network.hub_http_url);
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed (%s); continuing without playback", esp_err_to_name(audio_err));
    }
    ESP_ERROR_CHECK(dbmap_mqtt_start(&s_cfg, output_handle_command, &s_cfg));
    dbmap_ota_mark_valid_after_mqtt(&s_cfg);
    dbmap_desired_poll_start(&s_cfg);

    int backoff_ms = 1000;
    while (1) {
        dbmap_ota_report_pending_boot(&s_cfg);
        if (dbmap_publish_hello(&s_cfg) == ESP_OK) {
            backoff_ms = 1000;
        }
        dbmap_publish_health(&s_cfg);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms < 120000 ? backoff_ms : 120000));
        if (backoff_ms < 120000) {
            backoff_ms *= 2;
        }
        /* Output keeps a persistent session; health is frequent. Ear 24h poll is later. */
        backoff_ms = 30000;
    }
}
