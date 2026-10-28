#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Explicit OTA-reboot-pending gate.
 *
 * Set after a successful OTA install report (and bounded PUBACK wait). While
 * set, Wi-Fi/MQTT must not start new reconnect attempts, and diagnostic
 * counters must not treat the intentional teardown as a new incident.
 * Cleared only by reboot (RAM). No flash writes.
 */
void dbmap_lifecycle_enter_ota_reboot_pending(void);
bool dbmap_lifecycle_ota_reboot_pending(void);

/** Test helper only. */
void dbmap_lifecycle_reset_for_test(void);

#ifdef __cplusplus
}
#endif
