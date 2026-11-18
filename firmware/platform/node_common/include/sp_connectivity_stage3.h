#pragma once

/**
 * Stage 3: controlled Wi-Fi interface recovery (Sensor Platform).
 *
 * Escalates only after a long period without verified MQTT progress.
 * Does not reboot (Stage 4). Does not run through a stalled sp_net worker.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 15 minutes without verified progress. Longer than many 60 s backoff
 * cycles plus Stage 1/2 MQTT recovery attempts (proposal Stage 3 trigger).
 */
#define SP_CONN_STAGE3_ESCALATE_MS (15u * 60u * 1000u)
/** Minimum spacing between Stage 3 interface resets. */
#define SP_CONN_STAGE3_COOLDOWN_MS (5u * 60u * 1000u)
/** Bound for stop/start/connect/DHCP phases of one operation. */
#define SP_CONN_STAGE3_OP_TIMEOUT_MS (60u * 1000u)
/** After IP, wait for verified MQTT progress (Hub window). */
#define SP_CONN_STAGE3_MQTT_WAIT_MS 90000u
/** Consecutive failed Stage 3 ops before STAGE3_EXHAUSTED. */
#define SP_CONN_STAGE3_MAX_ATTEMPTS 3u
/** No Stage 3 until the supervisor has been up this long. */
#define SP_CONN_STAGE3_BOOT_GRACE_MS (2u * 60u * 1000u)

typedef enum {
    SP_CONN_S3_IDLE = 0,
    SP_CONN_S3_ACTIVE,
    SP_CONN_S3_COOLDOWN,
    SP_CONN_S3_EXHAUSTED,
    SP_CONN_S3_BLOCKED_BY_WORKER,
} sp_conn_s3_state_t;

typedef enum {
    SP_CONN_S3_PHASE_NONE = 0,
    SP_CONN_S3_PHASE_STOP,
    SP_CONN_S3_PHASE_START,
    SP_CONN_S3_PHASE_CONNECT,
    SP_CONN_S3_PHASE_WAIT_IP,
    SP_CONN_S3_PHASE_WAIT_MQTT,
} sp_conn_s3_phase_t;

typedef enum {
    SP_CONN_S3_RESULT_NONE = 0,
    SP_CONN_S3_RESULT_SUCCESS,
    SP_CONN_S3_RESULT_STOP_FAIL,
    SP_CONN_S3_RESULT_START_FAIL,
    SP_CONN_S3_RESULT_DHCP_TIMEOUT,
    SP_CONN_S3_RESULT_MQTT_TIMEOUT,
    SP_CONN_S3_RESULT_OP_TIMEOUT,
    SP_CONN_S3_RESULT_BLOCKED,
    SP_CONN_S3_RESULT_BUSY,
} sp_conn_s3_result_t;

typedef enum {
    SP_CONN_S3_ACTION_NONE = 0,
    /** Worker is responsive: run esp_wifi_stop/start on sp_net. */
    SP_CONN_S3_ACTION_BEGIN_IFACE_RESTART,
    /** Escalation due but worker stalled — detect only. */
    SP_CONN_S3_ACTION_RECORD_BLOCKED,
} sp_conn_s3_action_t;

typedef struct {
    int64_t now_ms;
    /** Time since supervisor start (monotonic). */
    int64_t supervisor_up_ms;
    /** 0 = never verified since boot. */
    int64_t last_verified_ms;
    /** False when Stage 2 has classified sp_net as stalled. */
    bool worker_responsive;
} sp_conn_s3_input_t;

typedef struct {
    sp_conn_s3_state_t state;
    sp_conn_s3_phase_t phase;
    sp_conn_s3_action_t action;
    sp_conn_s3_result_t last_result;
    uint32_t iface_generation;
    uint32_t attempt_count;
    uint32_t consecutive_failures;
    int64_t op_started_ms;
    int64_t phase_started_ms;
    int64_t cooldown_until_ms;
    bool stop_completed;
    bool start_completed;
    bool got_ip;
    bool mqtt_verified;
} sp_conn_s3_sm_t;

typedef struct {
    sp_conn_s3_state_t state;
    sp_conn_s3_phase_t phase;
    sp_conn_s3_action_t action;
    sp_conn_s3_result_t last_result;
    uint32_t iface_generation;
} sp_conn_s3_eval_t;

void sp_conn_s3_sm_init(sp_conn_s3_sm_t *sm);

/**
 * Decide whether to start Stage 3, or advance timeouts while ACTIVE.
 * Does not perform Wi-Fi driver calls.
 */
sp_conn_s3_eval_t sp_conn_s3_evaluate(sp_conn_s3_sm_t *sm, const sp_conn_s3_input_t *in);

/** Call when BEGIN action is accepted by the supervisor (worker will run). */
void sp_conn_s3_note_begin(sp_conn_s3_sm_t *sm, int64_t now_ms);

void sp_conn_s3_on_stop_ok(sp_conn_s3_sm_t *sm, int64_t now_ms);
void sp_conn_s3_on_stop_fail(sp_conn_s3_sm_t *sm, int64_t now_ms);
void sp_conn_s3_on_start_ok(sp_conn_s3_sm_t *sm, int64_t now_ms);
void sp_conn_s3_on_start_fail(sp_conn_s3_sm_t *sm, int64_t now_ms);
void sp_conn_s3_on_connect_started(sp_conn_s3_sm_t *sm, int64_t now_ms);

/**
 * GOT_IP for iface_generation. Stale generations are ignored for Stage 3
 * completion (return false).
 */
bool sp_conn_s3_on_got_ip(
    sp_conn_s3_sm_t *sm,
    uint32_t iface_generation,
    int64_t now_ms
);

void sp_conn_s3_on_mqtt_verified(sp_conn_s3_sm_t *sm, int64_t now_ms);

/** Verified progress outside an active op resets consecutive failures. */
void sp_conn_s3_on_verified_progress_idle(sp_conn_s3_sm_t *sm);

#ifdef __cplusplus
}
#endif
