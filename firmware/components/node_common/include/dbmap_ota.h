#pragma once

#include <stddef.h>
#include <stdint.h>

#include "dbmap_types.h"
#include "esp_err.h"

/**
 * Optional Ear acoustic-close hook invoked from persist_pending_and_reboot
 * before the install status publish / reboot prep.
 *
 * Output never registers; NULL means intentional no-op.
 * Ear must register a non-NULL callback before any OTA-capable Desired path.
 */
typedef esp_err_t (*dbmap_ota_acoustic_close_fn)(void);

/**
 * Install the Ear acoustic-close callback. fn must be non-NULL.
 * Returns ESP_OK on success. Does not use weak symbols.
 */
esp_err_t dbmap_ota_register_acoustic_close(dbmap_ota_acoustic_close_fn fn);

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
