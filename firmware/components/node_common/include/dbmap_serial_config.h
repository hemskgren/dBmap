#pragma once

#include "dbmap_types.h"
#include "esp_err.h"

/**
 * Default USB slice length for first-config / unprovisioned waits (seconds).
 * Accepted configure JSON still reboots immediately inside the wait.
 * Missing-AP recovery uses an indefinite Wi-Fi+USB wait instead of rebooting.
 */
#ifndef DBMAP_USB_BOOT_RECOVERY_S
#define DBMAP_USB_BOOT_RECOVERY_S 180
#endif

/* timeout_seconds == 0 waits indefinitely; otherwise returns ESP_ERR_TIMEOUT
 * when the window expires. */
esp_err_t dbmap_serial_config_wait(dbmap_effective_t *cfg, int timeout_seconds);

/**
 * After a failed first Wi-Fi join: wait until STA has IP, while still accepting
 * USB configure (reboots on accept). Wi-Fi STA reconnect runs in the background
 * with exponential backoff. timeout_seconds <= 0 waits indefinitely (preferred
 * when the configured SSID may be temporarily absent — do not reboot solely for
 * NO_AP_FOUND). timeout_seconds > 0 returns ESP_ERR_TIMEOUT when the bound expires.
 */
esp_err_t dbmap_boot_wait_wifi_ip_or_usb(dbmap_effective_t *cfg, int timeout_seconds);
