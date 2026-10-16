#include "dbmap_time.h"

#include <stdbool.h>
#include <time.h>

#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

static bool s_sntp_initialized;

esp_err_t dbmap_time_sync(uint32_t timeout_ms)
{
    if (!s_sntp_initialized) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_DBMAP_NTP_SERVER);
        esp_err_t err = esp_netif_sntp_init(&config);
        if (err != ESP_OK) return err;
        s_sntp_initialized = true;
    }
    return esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
}

esp_err_t dbmap_time_format_utc(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) return ESP_ERR_INVALID_ARG;

    time_t now;
    time(&now);
    if (now < 1577836800) return ESP_ERR_INVALID_STATE;

    struct tm utc_time;
    if (gmtime_r(&now, &utc_time) == NULL) return ESP_FAIL;
    return strftime(buffer, buffer_size, "%Y-%m-%dT%H:%M:%SZ", &utc_time) > 0
        ? ESP_OK
        : ESP_ERR_INVALID_SIZE;
}
