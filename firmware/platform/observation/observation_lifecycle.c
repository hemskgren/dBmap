#include "sp_observation_lifecycle.h"

bool sp_obs_gate_allows_feed(sp_obs_emit_gate_t gate)
{
    return gate == SP_OBS_GATE_RUN;
}

sp_obs_ota_pub_outcome_t sp_obs_ota_classify_publish(
    bool attempted_end,
    bool mqtt_connected,
    sp_obs_mqtt_wait_result_t wait_result
)
{
    if (!attempted_end) {
        return SP_OBS_OTA_PUB_NO_END;
    }
    if (!mqtt_connected) {
        return SP_OBS_OTA_PUB_SKIPPED_DISCONNECTED;
    }
    switch (wait_result) {
    case SP_OBS_MQTT_WAIT_OK:
        return SP_OBS_OTA_PUB_OK_PUBACK;
    case SP_OBS_MQTT_WAIT_TIMEOUT:
        return SP_OBS_OTA_PUB_TIMEOUT;
    case SP_OBS_MQTT_WAIT_FAILED:
    default:
        return SP_OBS_OTA_PUB_FAILED;
    }
}

const char *sp_obs_ota_pub_outcome_name(sp_obs_ota_pub_outcome_t outcome)
{
    switch (outcome) {
    case SP_OBS_OTA_PUB_NO_END:
        return "no_end";
    case SP_OBS_OTA_PUB_SKIPPED_DISCONNECTED:
        return "skipped_disconnected";
    case SP_OBS_OTA_PUB_OK_PUBACK:
        return "puback";
    case SP_OBS_OTA_PUB_TIMEOUT:
        return "puback_timeout";
    case SP_OBS_OTA_PUB_FAILED:
        return "publish_failed";
    default:
        return "unknown";
    }
}
