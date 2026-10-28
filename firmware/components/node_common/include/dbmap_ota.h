#pragma once

#include <stddef.h>
#include <stdint.h>

#include "dbmap_types.h"
#include "esp_err.h"

/* Download Desired esp-output.bin into the inactive OTA slot. Fail closed: abort keeps running image. */
esp_err_t dbmap_ota_apply(
    const dbmap_effective_t *cfg,
    const char *firmware_id,
    const char *firmware_version,
    const char *app_sha256_hex,
    uint32_t app_size_bytes
);

/**
 * After Wi-Fi + MQTT are up, confirm a pending-verify image so bootloader
 * rollback is cancelled, then report any pending OTA boot confirmation.
 */
void dbmap_ota_mark_valid_after_mqtt(const dbmap_effective_t *cfg);

/**
 * If NVS has a pending OTA record and MQTT is up, publish boot_confirmed /
 * boot_mismatch and clear the record. Safe to call repeatedly.
 */
void dbmap_ota_report_pending_boot(const dbmap_effective_t *cfg);
