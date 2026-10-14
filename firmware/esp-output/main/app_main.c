#include <string.h>

#include "dbmap_health.h"
#include "dbmap_mqtt.h"
#include "dbmap_nvs.h"
#include "dbmap_provision.h"
#include "dbmap_serial_config.h"
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
    dbmap_effective_defaults(&s_cfg);
    ESP_ERROR_CHECK(dbmap_nvs_load(&s_cfg));

    ESP_LOGI(
        TAG,
        "DBMAP_DEVICE_ID=%s",
        s_cfg.identity.node_id[0] != '\0' ? s_cfg.identity.node_id : "UNPROVISIONED"
    );
    ESP_LOGI(TAG, "boot hardware=%s provisioned=%d", s_cfg.identity.hardware_revision, (int)s_cfg.provisioned);

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

    if (!s_cfg.provisioned) {
        esp_err_t perr = dbmap_hub_provision(&s_cfg);
        if (perr != ESP_OK) {
            ESP_LOGE(TAG, "secure provisioning failed; waiting for corrected USB configuration");
            ESP_ERROR_CHECK(dbmap_serial_config_wait(&s_cfg, 0));
            esp_restart();
        }
    }

    ESP_ERROR_CHECK(dbmap_mqtt_start(&s_cfg, output_handle_command, &s_cfg));

    int backoff_ms = 1000;
    while (1) {
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
