#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Idempotent mDNS stack start (ESP-IDF mdns_init).
 * Call before the first *.local lookup when bring-up skipped init.
 */
esp_err_t sp_mdns_ensure(void);

#ifdef __cplusplus
}
#endif
