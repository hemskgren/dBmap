#include "sp_mdns.h"

#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "sp_mdns";

esp_err_t sp_mdns_ensure(void)
{
    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "mDNS started for *.local lookup");
        return ESP_OK;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        /* Already running. */
        return ESP_OK;
    }
    ESP_LOGW(TAG, "mDNS init failed: %s; *.local names may not resolve", esp_err_to_name(err));
    return err;
}
