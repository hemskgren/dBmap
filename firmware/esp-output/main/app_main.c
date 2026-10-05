#include <string.h>

#include "dbmap_health.h"
#include "dbmap_mqtt.h"
#include "dbmap_nvs.h"
#include "dbmap_provision.h"
#include "dbmap_types.h"
#include "dbmap_wifi.h"
#include "esp_log.h"
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

    ESP_LOGI(TAG, "boot hardware=%s provisioned=%d", s_cfg.identity.hardware_revision, (int)s_cfg.provisioned);

    ESP_ERROR_CHECK(dbmap_wifi_start(&s_cfg.network));

    if (!s_cfg.provisioned) {
        esp_err_t perr = dbmap_hub_provision(&s_cfg);
        if (perr != ESP_OK) {
            ESP_LOGE(TAG, "secure provisioning failed; refusing lab MQTT fallback");
            ESP_ERROR_CHECK(perr);
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
