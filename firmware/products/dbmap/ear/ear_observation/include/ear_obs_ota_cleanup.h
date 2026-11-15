/**
 * Product-side OTA acoustic cleanup helpers (host-testable).
 *
 * ear_observation_close_for_ota() walks Platform registry slots for normal
 * Observations, then calls ear_obs_ota_release_product_bindings() so Critical
 * Path is released even when no registry slots are in use.
 */
#pragma once

#include "ear_critical_path.h"
#include "ear_obs_manager.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Release EAR product bindings that are not Platform registry slots.
 * Order matches production OTA close: forget normal manager bindings (if any),
 * then terminate Critical (if any). Critical cleanup does not depend on
 * registry occupancy.
 */
void ear_obs_ota_release_product_bindings(
    ear_obs_manager_t *om_or_null,
    ear_critical_path_t *critical_or_null,
    int64_t now_mono_us
);

#ifdef __cplusplus
}
#endif
