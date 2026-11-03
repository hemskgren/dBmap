#include "dbmap_observation_lifecycle.h"

bool dbmap_obs_gate_allows_feed(dbmap_obs_emit_gate_t gate)
{
    return gate == DBMAP_OBS_GATE_RUN;
}

dbmap_obs_ota_action_t dbmap_obs_ota_action_for_state(
    dbmap_activity_state_t state,
    bool start_publish_accepted
)
{
    switch (state) {
    case DBMAP_ACTIVITY_IDLE:
        return DBMAP_OBS_OTA_ACTION_NOOP;
    case DBMAP_ACTIVITY_PENDING_ENTER:
        (void)start_publish_accepted;
        return DBMAP_OBS_OTA_ACTION_SILENT_ABORT;
    case DBMAP_ACTIVITY_ACTIVE:
    case DBMAP_ACTIVITY_PENDING_EXIT:
        return start_publish_accepted ? DBMAP_OBS_OTA_ACTION_FORCE_END
                                      : DBMAP_OBS_OTA_ACTION_SILENT_ABORT;
    default:
        return DBMAP_OBS_OTA_ACTION_SILENT_ABORT;
    }
}

dbmap_obs_ota_pub_outcome_t dbmap_obs_ota_classify_publish(
    bool attempted_end,
    bool mqtt_connected,
    dbmap_obs_mqtt_wait_result_t wait_result
)
{
    if (!attempted_end) {
        return DBMAP_OBS_OTA_PUB_NO_END;
    }
    if (!mqtt_connected) {
        return DBMAP_OBS_OTA_PUB_SKIPPED_DISCONNECTED;
    }
    switch (wait_result) {
    case DBMAP_OBS_MQTT_WAIT_OK:
        return DBMAP_OBS_OTA_PUB_OK_PUBACK;
    case DBMAP_OBS_MQTT_WAIT_TIMEOUT:
        return DBMAP_OBS_OTA_PUB_TIMEOUT;
    case DBMAP_OBS_MQTT_WAIT_FAILED:
    default:
        return DBMAP_OBS_OTA_PUB_FAILED;
    }
}

const char *dbmap_obs_ota_pub_outcome_name(dbmap_obs_ota_pub_outcome_t outcome)
{
    switch (outcome) {
    case DBMAP_OBS_OTA_PUB_NO_END:
        return "no_end";
    case DBMAP_OBS_OTA_PUB_SKIPPED_DISCONNECTED:
        return "skipped_disconnected";
    case DBMAP_OBS_OTA_PUB_OK_PUBACK:
        return "puback";
    case DBMAP_OBS_OTA_PUB_TIMEOUT:
        return "puback_timeout";
    case DBMAP_OBS_OTA_PUB_FAILED:
        return "publish_failed";
    default:
        return "unknown";
    }
}
