#pragma once

/**
 * Stage 4: controlled reboot after prolonged lack of verified MQTT progress.
 *
 * Final escalation after Stages 1–3. No NVS reboot budget. No Observation END
 * before reboot. No Hub/protocol changes. No dependency on OTA acoustic-close.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 60 minutes without verified MQTT progress (boot or since last verify).
 * Longer than Stage 3 escalate (15 min) + several Stage 3 attempts/cooldowns
 * so Stages 1–3 have a genuine opportunity first.
 */
#define SP_CONN_STAGE4_NO_PROGRESS_MS (60u * 60u * 1000u)

/** Match OTA teardown settle before esp_restart. */
#define SP_CONN_STAGE4_REBOOT_CLEANUP_MS 500u

typedef enum {
    SP_CONN_S4_IDLE = 0,
    SP_CONN_S4_REBOOT_PENDING,
} sp_conn_s4_state_t;

typedef enum {
    SP_CONN_S4_ACTION_NONE = 0,
    /** Proceed to controlled esp_restart (no Observation END). */
    SP_CONN_S4_ACTION_EXECUTE_REBOOT,
} sp_conn_s4_action_t;

typedef struct {
    int64_t now_ms;
    /**
     * 0 = never verified since boot. Age uses now_ms (esp_timer from boot)
     * when last_verified_ms <= 0; otherwise now_ms - last_verified_ms.
     */
    int64_t last_verified_ms;
    /** True while OTA reboot-pending already owns the reboot. */
    bool reboot_pending_other;
} sp_conn_s4_input_t;

typedef struct {
    sp_conn_s4_state_t state;
} sp_conn_s4_sm_t;

typedef struct {
    sp_conn_s4_state_t state;
    sp_conn_s4_action_t action;
    int64_t no_progress_age_ms;
} sp_conn_s4_eval_t;

void sp_conn_s4_sm_init(sp_conn_s4_sm_t *sm);

/** Age since last verified progress, or since boot if never verified. */
int64_t sp_conn_s4_no_progress_age_ms(const sp_conn_s4_input_t *in);

sp_conn_s4_eval_t sp_conn_s4_evaluate(sp_conn_s4_sm_t *sm, const sp_conn_s4_input_t *in);

/** Verified MQTT progress: cancel Stage 4 and reset the 60-minute window. */
void sp_conn_s4_on_mqtt_verified(sp_conn_s4_sm_t *sm, int64_t now_ms);

#ifdef __cplusplus
}
#endif
