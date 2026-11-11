#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Observation emission lifecycle gate (Ear activity task / OTA close). */
typedef enum {
    SP_OBS_GATE_RUN = 0,
    SP_OBS_GATE_CLOSING,
    SP_OBS_GATE_CLOSED,
} sp_obs_emit_gate_t;

/** OTA close decision from detector state + START client-accept flag. */
typedef enum {
    SP_OBS_OTA_ACTION_NOOP = 0,
    SP_OBS_OTA_ACTION_SILENT_ABORT,
    SP_OBS_OTA_ACTION_FORCE_END,
} sp_obs_ota_action_t;

/** Raw MQTT wait classification input (host-testable; map from esp_err_t on device). */
typedef enum {
    SP_OBS_MQTT_WAIT_OK = 0,
    SP_OBS_MQTT_WAIT_TIMEOUT,
    SP_OBS_MQTT_WAIT_FAILED,
} sp_obs_mqtt_wait_result_t;

/**
 * Classify OTA END MQTT wait results for logging.
 * PUBACK success does not prove Hub ingest.
 */
typedef enum {
    SP_OBS_OTA_PUB_NO_END = 0,
    SP_OBS_OTA_PUB_SKIPPED_DISCONNECTED,
    SP_OBS_OTA_PUB_OK_PUBACK,
    SP_OBS_OTA_PUB_TIMEOUT,
    SP_OBS_OTA_PUB_FAILED,
} sp_obs_ota_pub_outcome_t;

/** True only while gate is RUN (activity processing allowed). */
bool sp_obs_gate_allows_feed(sp_obs_emit_gate_t gate);

/**
 * Map MQTT connected flag + wait result to an outcome.
 * wait_result is ignored when connected is false or when no end was attempted.
 */
sp_obs_ota_pub_outcome_t sp_obs_ota_classify_publish(
    bool attempted_end,
    bool mqtt_connected,
    sp_obs_mqtt_wait_result_t wait_result
);

const char *sp_obs_ota_pub_outcome_name(sp_obs_ota_pub_outcome_t outcome);

#ifdef __cplusplus
}
#endif
