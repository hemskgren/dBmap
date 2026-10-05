#pragma once

#include "dbmap_types.h"
#include "esp_err.h"

esp_err_t dbmap_nvs_init(void);
esp_err_t dbmap_nvs_load(dbmap_effective_t *cfg);
esp_err_t dbmap_nvs_save_identity_network(const dbmap_effective_t *cfg);
esp_err_t dbmap_nvs_save_bootstrap_token(const char *token);
