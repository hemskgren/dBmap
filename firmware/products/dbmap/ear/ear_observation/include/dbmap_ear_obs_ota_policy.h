#pragma once

#include <stdbool.h>

#include "dbmap_activity_detector.h"
#include "sp_observation_lifecycle.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Approved OTA close state table (Ear activity detector):
 * IDLE → NOOP
 * PENDING_ENTER → SILENT_ABORT (no START produced)
 * ACTIVE / PENDING_EXIT + start accepted → FORCE_END
 * ACTIVE / PENDING_EXIT + start not accepted → SILENT_ABORT
 */
sp_obs_ota_action_t sp_obs_ota_action_for_state(
    dbmap_activity_state_t state,
    bool start_publish_accepted
);

#ifdef __cplusplus
}
#endif
