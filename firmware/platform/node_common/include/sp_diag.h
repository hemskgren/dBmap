#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * RAM-only device diagnostics (D1).
 *
 * Defaults come from Kconfig and can later be overridden by Desired
 * `payload.diag` when a configuration sync bumps `config_version`.
 * Counters are not written to NVS; they clear on reboot.
 *
 * Counter meanings:
 * - wifi_disconnects: WIFI_EVENT_STA_DISCONNECTED observations
 * - mqtt_disconnects: transitions from MQTT connected → disconnected
 * - mqtt_reconnects: successful MQTT CONNECT after a prior disconnect
 * - mqtt_connect_attempts: MQTT connect/reconnect starts (incl. first start)
 * - mqtt_connect_failures: connect/reconnect endings that did not reach connected
 * - desired_failures: Desired HTTPS checks that failed (transport or HTTP)
 */
typedef struct {
    bool enabled;
    int rssi_degraded_dbm;
    uint16_t counter_saturate;
} sp_diag_config_t;

void sp_diag_init(void);
void sp_diag_config_defaults(sp_diag_config_t *out);

/** Merge optional Desired `payload.diag` object; unknown keys ignored. */
void sp_diag_apply_desired_payload(const cJSON *payload);

const sp_diag_config_t *sp_diag_config(void);

void sp_diag_note_wifi_disconnect(void);
void sp_diag_note_mqtt_disconnect(void);
void sp_diag_note_mqtt_reconnect(void);
void sp_diag_note_mqtt_connect_attempt(void);
void sp_diag_note_mqtt_connect_failure(void);
void sp_diag_note_desired_failure(void);

/** Observation MQTT publish diagnostics (RAM-only; omit-when-zero on wire). */
void sp_diag_note_obs_start_rejected(void);
void sp_diag_note_obs_periodic_rejected(void);
void sp_diag_note_obs_end_rejected(void);
void sp_diag_note_obs_complete_rejected(void);
void sp_diag_note_obs_skipped_disconnected(void);

/**
 * Attach a compact `diag` object when enabled and there is something to report.
 * After a successful MQTT publish of health/keepalive, call
 * sp_diag_clear_oneshots() so reset reason is sent at most once per boot.
 */
void sp_diag_attach_json(cJSON *state_object);
void sp_diag_clear_oneshots(void);

#ifdef __cplusplus
}
#endif
