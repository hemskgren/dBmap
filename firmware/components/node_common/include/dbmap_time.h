#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t dbmap_time_sync(uint32_t timeout_ms);
esp_err_t dbmap_time_format_utc(char *buffer, size_t buffer_size);

/**
 * @brief Return the current UTC wall-clock time as epoch microseconds.
 *
 * Returns ESP_ERR_INVALID_STATE if the system clock has not been
 * synchronised yet (i.e. time is before 2020-01-01).
 */
esp_err_t dbmap_time_get_epoch_us(int64_t *out_us);

/**
 * @brief Parse an ISO-8601 UTC timestamp ("YYYY-MM-DDTHH:MM:SSZ") to
 *        epoch microseconds.
 *
 * Returns ESP_ERR_INVALID_ARG if the string cannot be parsed.
 */
esp_err_t dbmap_time_parse_iso8601(const char *iso, int64_t *out_us);
