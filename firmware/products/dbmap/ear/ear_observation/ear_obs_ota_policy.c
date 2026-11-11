#include "dbmap_ear_obs_ota_policy.h"

sp_obs_ota_action_t sp_obs_ota_action_for_state(
    dbmap_activity_state_t state,
    bool start_publish_accepted
)
{
    switch (state) {
    case DBMAP_ACTIVITY_IDLE:
        return SP_OBS_OTA_ACTION_NOOP;
    case DBMAP_ACTIVITY_PENDING_ENTER:
        (void)start_publish_accepted;
        return SP_OBS_OTA_ACTION_SILENT_ABORT;
    case DBMAP_ACTIVITY_ACTIVE:
    case DBMAP_ACTIVITY_PENDING_EXIT:
        return start_publish_accepted ? SP_OBS_OTA_ACTION_FORCE_END
                                      : SP_OBS_OTA_ACTION_SILENT_ABORT;
    default:
        return SP_OBS_OTA_ACTION_SILENT_ABORT;
    }
}
