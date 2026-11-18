#include "sp_connectivity.h"

#include <stddef.h>

void sp_conn_sm_init(sp_conn_sm_t *sm)
{
    if (sm == NULL) {
        return;
    }
    sm->stage = SP_CONN_STAGE_OK;
    sm->stage_since_ms = 0;
    sm->timer_arm_retries = 0;
    sm->recovery_request_active = false;
    sm->last_fail = SP_CONN_FAIL_NONE;
}

void sp_conn_sm_note_recovery_started(sp_conn_sm_t *sm)
{
    if (sm != NULL) {
        sm->recovery_request_active = true;
    }
}

void sp_conn_sm_note_recovery_finished(sp_conn_sm_t *sm)
{
    if (sm != NULL) {
        sm->recovery_request_active = false;
    }
}

void sp_conn_sm_note_timer_retry(sp_conn_sm_t *sm)
{
    if (sm == NULL) {
        return;
    }
    if (sm->timer_arm_retries < UINT32_MAX) {
        sm->timer_arm_retries++;
    }
}

void sp_conn_sm_clear_timer_retries(sp_conn_sm_t *sm)
{
    if (sm != NULL) {
        sm->timer_arm_retries = 0;
    }
}

static void set_stage(
    sp_conn_sm_t *sm,
    sp_conn_sm_result_t *out,
    const sp_conn_sm_input_t *in,
    sp_conn_stage_t stage,
    sp_conn_action_t action,
    sp_conn_fail_t fail
)
{
    if (sm->stage != stage) {
        sm->stage = stage;
        sm->stage_since_ms = in->now_ms;
    }
    out->stage = stage;
    out->action = action;
    out->fail_reason = fail;
    if (fail != SP_CONN_FAIL_NONE) {
        sm->last_fail = fail;
    }
}

sp_conn_sm_result_t sp_conn_sm_evaluate(sp_conn_sm_t *sm, const sp_conn_sm_input_t *in)
{
    sp_conn_sm_result_t out = {
        .stage = SP_CONN_STAGE_OK,
        .action = SP_CONN_ACTION_NONE,
        .fail_reason = SP_CONN_FAIL_NONE,
    };
    if (sm == NULL || in == NULL) {
        return out;
    }

    if (!in->supervisor_started) {
        set_stage(
            sm, &out, in, SP_CONN_STAGE_INIT_FAILED, SP_CONN_ACTION_NONE,
            SP_CONN_FAIL_SUPERVISOR_TASK
        );
        return out;
    }

    if (!in->worker_started) {
        set_stage(
            sm, &out, in, SP_CONN_STAGE_INIT_FAILED, SP_CONN_ACTION_NONE,
            SP_CONN_FAIL_WORKER_TASK
        );
        return out;
    }

    /* Stalled worker: only while work is pending. Idle backoff must not trip. */
    if (in->dispatch_pending
        && in->worker_heartbeat_age_ms >= 0
        && (uint32_t)in->worker_heartbeat_age_ms >= SP_CONN_WORKER_STALL_MS) {
        set_stage(
            sm, &out, in, SP_CONN_STAGE_WORKER_STALLED,
            SP_CONN_ACTION_RECORD_UNRECOVERABLE_STALL, SP_CONN_FAIL_NONE
        );
        return out;
    }

    /* Heartbeat fresh but pending work never completes. */
    if (in->dispatch_pending
        && in->worker_heartbeat_age_ms >= 0
        && (uint32_t)in->worker_heartbeat_age_ms < SP_CONN_WORKER_STALL_MS
        && in->pending_age_ms >= 0
        && (uint32_t)in->pending_age_ms >= SP_CONN_DISPATCH_STALL_MS
        && in->last_dispatch_age_ms >= 0
        && in->last_dispatch_age_ms >= in->pending_age_ms) {
        set_stage(
            sm, &out, in, SP_CONN_STAGE_DISPATCH_STALLED,
            SP_CONN_ACTION_RECORD_UNRECOVERABLE_STALL, SP_CONN_FAIL_NONE
        );
        return out;
    }

    if (in->timer_arm_failed) {
        sp_conn_action_t action = SP_CONN_ACTION_NONE;
        if (in->timer_arm_retries < SP_CONN_TIMER_RETRY_MAX
            && in->worker_heartbeat_age_ms >= 0
            && (uint32_t)in->worker_heartbeat_age_ms < SP_CONN_WORKER_STALL_MS) {
            action = SP_CONN_ACTION_RETRY_TIMER_PATH;
        }
        set_stage(
            sm, &out, in, SP_CONN_STAGE_TIMER_ARM_FAULT, action, sm->last_fail
        );
        return out;
    }

    if (!in->wifi_has_ip) {
        sm->recovery_request_active = false;
        set_stage(
            sm, &out, in, SP_CONN_STAGE_NO_IP, SP_CONN_ACTION_NONE,
            SP_CONN_FAIL_NONE
        );
        return out;
    }

    if (in->mqtt_connected && in->mqtt_progress_expired) {
        sp_conn_action_t action = SP_CONN_ACTION_NONE;
        if (!in->recovery_request_active
            && in->worker_heartbeat_age_ms >= 0
            && (uint32_t)in->worker_heartbeat_age_ms < SP_CONN_WORKER_STALL_MS) {
            action = SP_CONN_ACTION_REQUEST_MQTT_RECOVERY;
        } else if (in->recovery_request_active) {
            action = SP_CONN_ACTION_NONE;
        } else {
            action = SP_CONN_ACTION_RECORD_UNRECOVERABLE_STALL;
        }
        set_stage(
            sm, &out, in, SP_CONN_STAGE_MQTT_NO_PROGRESS, action,
            SP_CONN_FAIL_NONE
        );
        return out;
    }

    if (!in->mqtt_connected) {
        if (in->mqtt_backoff_waiting) {
            sm->recovery_request_active = false;
            set_stage(
                sm, &out, in, SP_CONN_STAGE_MQTT_BACKOFF, SP_CONN_ACTION_NONE,
                SP_CONN_FAIL_NONE
            );
            return out;
        }
        /* Disconnected and ready / overdue for an attempt. */
        if (!in->recovery_request_active
            && in->worker_heartbeat_age_ms >= 0
            && (uint32_t)in->worker_heartbeat_age_ms < SP_CONN_WORKER_STALL_MS) {
            set_stage(
                sm, &out, in, SP_CONN_STAGE_MQTT_BACKOFF,
                SP_CONN_ACTION_REQUEST_MQTT_RECOVERY, SP_CONN_FAIL_NONE
            );
            return out;
        }
        set_stage(
            sm, &out, in, SP_CONN_STAGE_MQTT_BACKOFF, SP_CONN_ACTION_NONE,
            SP_CONN_FAIL_NONE
        );
        return out;
    }

    sm->recovery_request_active = false;
    set_stage(sm, &out, in, SP_CONN_STAGE_OK, SP_CONN_ACTION_NONE, SP_CONN_FAIL_NONE);
    return out;
}
