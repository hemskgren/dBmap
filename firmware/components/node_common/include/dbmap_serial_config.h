#pragma once

#include "dbmap_types.h"
#include "esp_err.h"

/* timeout_seconds == 0 waits indefinitely; otherwise returns ESP_ERR_TIMEOUT. */
esp_err_t dbmap_serial_config_wait(dbmap_effective_t *cfg, int timeout_seconds);
