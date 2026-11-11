#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sp_types.h"
#include "esp_err.h"

esp_err_t sp_nvs_init(void);
esp_err_t sp_nvs_load(sp_effective_t *cfg);
esp_err_t sp_nvs_save_identity_network(const sp_effective_t *cfg);
esp_err_t sp_nvs_save_bootstrap_token(const char *token);

/**
 * Minimal pending-OTA record for post-reboot confirmation.
 * One NVS commit only; cleared after the boot report (or mismatch clear).
 */
typedef struct {
    char previous_version[32];
    char target_version[32];
    char firmware_id[64];
    bool pending;
} sp_ota_pending_t;

esp_err_t sp_nvs_ota_pending_save(const sp_ota_pending_t *pending);
esp_err_t sp_nvs_ota_pending_load(sp_ota_pending_t *out);
esp_err_t sp_nvs_ota_pending_clear(void);
