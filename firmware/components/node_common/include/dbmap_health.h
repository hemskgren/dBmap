#pragma once

#include "dbmap_types.h"
#include "esp_err.h"

esp_err_t dbmap_publish_hello(const dbmap_effective_t *cfg);
esp_err_t dbmap_publish_health(const dbmap_effective_t *cfg);
esp_err_t dbmap_publish_status(const dbmap_effective_t *cfg, const char *status_json);
