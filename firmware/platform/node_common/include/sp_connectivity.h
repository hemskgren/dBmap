#pragma once

/**
 * Connectivity supervisor (Sensor Platform) — Stages 2–4.
 *
 * Independent of sp_net for monitoring. Stage 3 may request a Wi-Fi
 * interface restart only on a responsive worker. Stage 4 may reboot after
 * 60 minutes without verified MQTT progress (no NVS reboot budget).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sp_connectivity_stage3.h"
#include "sp_connectivity_stage4.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Same Hub online window / Stage 1 progress deadline. */
#define SP_CONN_PROGRESS_LIMIT_MS 90000u
/**
 * Worker / dispatch stall bound. Matches the existing MQTT connect
 * watchdog and Wi-Fi in-flight timeout so a blocked stop/DNS is noticed
 * on the same scale as a hung connect, not after the 60 s backoff cap.
 */
#define SP_CONN_WORKER_STALL_MS 30000u
#define SP_CONN_DISPATCH_STALL_MS 30000u
/** Evaluation period. Shorter than publish (30 s) and stall (30 s). */
#define SP_CONN_SUPERVISOR_PERIOD_MS 5000u
/** Floor for deferred timer-arm retries (existing backoff initial). */
#define SP_CONN_TIMER_RETRY_MIN_MS 1000u
/** Cap deferred timer-arm retries before staying in degraded. */
#define SP_CONN_TIMER_RETRY_MAX 5u

typedef enum {
    SP_CONN_STAGE_OK = 0,
    SP_CONN_STAGE_NO_IP,
    /** MQTT down and next attempt is correctly deferred (not a stall). */
    SP_CONN_STAGE_MQTT_BACKOFF,
    SP_CONN_STAGE_MQTT_NO_PROGRESS,
    SP_CONN_STAGE_WORKER_STALLED,
    SP_CONN_STAGE_DISPATCH_STALLED,
    SP_CONN_STAGE_TIMER_ARM_FAULT,
    SP_CONN_STAGE_INIT_FAILED,
} sp_conn_stage_t;

typedef enum {
    SP_CONN_ACTION_NONE = 0,
    /** Worker heartbeat is fresh: safe to post MQTT recovery. */
    SP_CONN_ACTION_REQUEST_MQTT_RECOVERY,
    /** Re-drive a timer path through dispatch (bounded). */
    SP_CONN_ACTION_RETRY_TIMER_PATH,
    /** Detected only; Stage 2 must not kill the worker. */
    SP_CONN_ACTION_RECORD_UNRECOVERABLE_STALL,
} sp_conn_action_t;

typedef enum {
    SP_CONN_FAIL_NONE = 0,
    SP_CONN_FAIL_SUPERVISOR_TASK,
    SP_CONN_FAIL_WORKER_TASK,
    SP_CONN_FAIL_MQTT_RECONNECT_TIMER,
    SP_CONN_FAIL_MQTT_PROGRESS_TIMER,
    SP_CONN_FAIL_MQTT_WATCHDOG_TIMER,
    SP_CONN_FAIL_MQTT_STABLE_TIMER,
    SP_CONN_FAIL_WIFI_RECONNECT_TIMER,
} sp_conn_fail_t;

typedef struct {
    int64_t now_ms;
    bool wifi_has_ip;
    bool mqtt_connected;
    bool mqtt_progress_expired;
    int64_t mqtt_progress_age_ms;
    bool mqtt_backoff_waiting;
    int64_t mqtt_next_attempt_in_ms;
    bool worker_started;
    bool dispatch_pending;
    int64_t worker_heartbeat_age_ms;
    int64_t last_dispatch_age_ms;
    int64_t pending_age_ms;
    bool recovery_request_active;
    bool timer_arm_failed;
    uint32_t timer_arm_retries;
    bool supervisor_started;
} sp_conn_sm_input_t;

typedef struct {
    sp_conn_stage_t stage;
    sp_conn_action_t action;
    sp_conn_fail_t fail_reason;
} sp_conn_sm_result_t;

typedef struct {
    sp_conn_stage_t stage;
    int64_t stage_since_ms;
    uint32_t timer_arm_retries;
    bool recovery_request_active;
    sp_conn_fail_t last_fail;
} sp_conn_sm_t;

typedef struct {
    bool wifi_has_ip;
    bool mqtt_connected;
    int64_t mqtt_progress_age_ms;
    sp_conn_stage_t stage;
    int64_t stage_age_ms;
    int64_t worker_heartbeat_age_ms;
    int64_t last_dispatch_age_ms;
    bool dispatch_pending;
    sp_conn_fail_t last_fail;
    bool supervisor_running;
    bool worker_started;
    /* Stage 3 */
    sp_conn_s3_state_t stage3_state;
    sp_conn_s3_phase_t stage3_phase;
    uint32_t stage3_attempts;
    uint32_t stage3_consecutive_failures;
    int64_t stage3_op_elapsed_ms;
    sp_conn_s3_result_t stage3_last_result;
    bool stage3_stop_completed;
    bool stage3_start_completed;
    bool stage3_got_ip;
    bool stage3_mqtt_verified;
    bool stage3_blocked_by_worker;
    uint32_t stage3_iface_generation;
    /* Stage 4 */
    sp_conn_s4_state_t stage4_state;
    int64_t stage4_no_progress_age_ms;
    bool stage4_reboot_armed;
} sp_conn_status_t;

void sp_conn_sm_init(sp_conn_sm_t *sm);
sp_conn_sm_result_t sp_conn_sm_evaluate(sp_conn_sm_t *sm, const sp_conn_sm_input_t *in);
void sp_conn_sm_note_recovery_started(sp_conn_sm_t *sm);
void sp_conn_sm_note_recovery_finished(sp_conn_sm_t *sm);
void sp_conn_sm_note_timer_retry(sp_conn_sm_t *sm);
void sp_conn_sm_clear_timer_retries(sp_conn_sm_t *sm);

/** Start supervisor once (from Wi-Fi / MQTT start). */
void sp_connectivity_start(void);

bool sp_connectivity_is_running(void);

void sp_connectivity_get_status(sp_conn_status_t *out);

void sp_connectivity_note_timer_arm_failed(sp_conn_fail_t reason);
void sp_connectivity_note_timer_arm_ok(void);

void sp_connectivity_note_mqtt_connected(bool connected);
void sp_connectivity_note_wifi_has_ip(bool has_ip);

/**
 * Optional MQTT hooks registered from hub_mqtt so the supervisor does not
 * reach into MQTT statics. Progress age/expiry use the Stage 1 tracker.
 */
void sp_connectivity_set_mqtt_hooks(
    int64_t (*progress_age_ms)(int64_t now_ms),
    bool (*progress_expired)(int64_t now_ms),
    int64_t (*next_attempt_in_ms)(int64_t now_ms),
    int64_t (*last_verified_ms)(void)
);

/** Notify verified MQTT progress (PUBACK/DATA) for Stage 3 success / reset. */
void sp_connectivity_note_mqtt_verified(void);

#ifdef __cplusplus
}
#endif
