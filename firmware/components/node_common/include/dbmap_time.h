#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t dbmap_time_sync(uint32_t timeout_ms);
esp_err_t dbmap_time_format_utc(char *buffer, size_t buffer_size);
