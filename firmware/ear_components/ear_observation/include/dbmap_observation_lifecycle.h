#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dbmap_activity_detector.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Observation emission lifecycle gate (Ear activity task / OTA close). */
typedef enum {
    DBMAP_OBS_GATE_RUN = 0,
    DBMAP_OBS_GATE_CLOSING,
    DBMAP_OBS_GATE_CLOSED,
} dbmap_obs_emit_gate_t;

/** OTA close decision from detector state + START client-accept flag. */
typedef enum {
    DBMAP_OBS_OTA_ACTION_NOOP = 0,
    DBMAP_OBS_OTA_ACTION_SILENT_ABORT,
    DBMAP_OBS_OTA_ACTION_FORCE_END,
} dbmap_obs_ota_action_t;

/** Raw MQTT wait classification input (host-testable; map from esp_err_t on device). */
typedef enum {
    DBMAP_OBS_MQTT_WAIT_OK = 0,
    DBMAP_OBS_MQTT_WAIT_TIMEOUT,
    DBMAP_OBS_MQTT_WAIT_FAILED,
} dbmap_obs_mqtt_wait_result_t;

/**
 * Classify OTA END MQTT wait results for logging.
 * PUBACK success does not prove Hub ingest.
 */
typedef enum {
    DBMAP_OBS_OTA_PUB_NO_END = 0,
    DBMAP_OBS_OTA_PUB_SKIPPED_DISCONNECTED,
    DBMAP_OBS_OTA_PUB_OK_PUBACK,
    DBMAP_OBS_OTA_PUB_TIMEOUT,
    DBMAP_OBS_OTA_PUB_FAILED,
} dbmap_obs_ota_pub_outcome_t;

/** True only while gate is RUN (activity processing allowed). */
bool dbmap_obs_gate_allows_feed(dbmap_obs_emit_gate_t gate);

/**
 * Approved OTA close state table:
 * IDLE → NOOP
 * PENDING_ENTER → SILENT_ABORT (no START produced)
 * ACTIVE / PENDING_EXIT + start accepted → FORCE_END
 * ACTIVE / PENDING_EXIT + start not accepted → SILENT_ABORT
 */
dbmap_obs_ota_action_t dbmap_obs_ota_action_for_state(
    dbmap_activity_state_t state,
    bool start_publish_accepted
);

/**
 * Map MQTT connected flag + wait result to an outcome.
 * wait_result is ignored when connected is false or when no end was attempted.
 */
dbmap_obs_ota_pub_outcome_t dbmap_obs_ota_classify_publish(
    bool attempted_end,
    bool mqtt_connected,
    dbmap_obs_mqtt_wait_result_t wait_result
);

const char *dbmap_obs_ota_pub_outcome_name(dbmap_obs_ota_pub_outcome_t outcome);

#ifdef __cplusplus
}
#endif
