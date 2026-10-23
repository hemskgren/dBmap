#include "dbmap_time.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

/* newlib on ESP-IDF does not declare GNU timegm(); convert UTC tm ourselves. */
static time_t dbmap_timegm(const struct tm *tm)
{
    static const int days_to_month[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };

    int year = tm->tm_year + 1900;
    int month = tm->tm_mon;
    if (month < 0 || month > 11 || tm->tm_mday < 1 || tm->tm_mday > 31) {
        return (time_t)-1;
    }

    int64_t y = year;
    int64_t days = (y - 1970) * 365
        + ((y - 1) / 4 - 1969 / 4)
        - ((y - 1) / 100 - 1969 / 100)
        + ((y - 1) / 400 - 1969 / 400)
        + days_to_month[month]
        + (tm->tm_mday - 1);
    if (month > 1 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
        days += 1;
    }

    return (time_t)(days * 86400LL
        + (int64_t)tm->tm_hour * 3600
        + (int64_t)tm->tm_min * 60
        + tm->tm_sec);
}

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

esp_err_t dbmap_time_get_epoch_us(int64_t *out_us)
{
    if (out_us == NULL) return ESP_ERR_INVALID_ARG;
    time_t now;
    time(&now);
    if (now < 1577836800) return ESP_ERR_INVALID_STATE;  /* clock not synced */
    *out_us = (int64_t)now * 1000000LL;
    return ESP_OK;
}

esp_err_t dbmap_time_parse_iso8601(const char *iso, int64_t *out_us)
{
    if (iso == NULL || out_us == NULL) return ESP_ERR_INVALID_ARG;
    struct tm t = {0};
    /* Accept "YYYY-MM-DDTHH:MM:SSZ" */
    const char *end = strptime(iso, "%Y-%m-%dT%H:%M:%S", &t);
    if (end == NULL) return ESP_ERR_INVALID_ARG;
    time_t epoch = dbmap_timegm(&t);
    if (epoch == (time_t)-1) return ESP_ERR_INVALID_ARG;
    *out_us = (int64_t)epoch * 1000000LL;
    return ESP_OK;
}
