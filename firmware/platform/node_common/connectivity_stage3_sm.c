#include "sp_connectivity_stage3.h"

#include <stddef.h>

void sp_conn_s3_sm_init(sp_conn_s3_sm_t *sm)
{
    if (sm == NULL) {
        return;
    }
    sm->state = SP_CONN_S3_IDLE;
    sm->phase = SP_CONN_S3_PHASE_NONE;
    sm->last_result = SP_CONN_S3_RESULT_NONE;
    sm->iface_generation = 0;
    sm->attempt_count = 0;
    sm->consecutive_failures = 0;
    sm->op_started_ms = 0;
    sm->phase_started_ms = 0;
    sm->cooldown_until_ms = 0;
    sm->stop_completed = false;
    sm->start_completed = false;
    sm->got_ip = false;
    sm->mqtt_verified = false;
}

static void finish_failure(
    sp_conn_s3_sm_t *sm,
    int64_t now_ms,
    sp_conn_s3_result_t result
)
{
    sm->last_result = result;
    sm->phase = SP_CONN_S3_PHASE_NONE;
    sm->stop_completed = false;
    sm->start_completed = false;
    sm->got_ip = false;
    sm->mqtt_verified = false;
    if (sm->consecutive_failures < UINT32_MAX) {
        sm->consecutive_failures++;
    }
    if (sm->consecutive_failures >= SP_CONN_STAGE3_MAX_ATTEMPTS) {
        sm->state = SP_CONN_S3_EXHAUSTED;
        sm->cooldown_until_ms = 0;
        return;
    }
    sm->state = SP_CONN_S3_COOLDOWN;
    sm->cooldown_until_ms = now_ms + (int64_t)SP_CONN_STAGE3_COOLDOWN_MS;
}

static void finish_success(sp_conn_s3_sm_t *sm, int64_t now_ms)
{
    (void)now_ms;
    sm->last_result = SP_CONN_S3_RESULT_SUCCESS;
    sm->phase = SP_CONN_S3_PHASE_NONE;
    sm->state = SP_CONN_S3_IDLE;
    sm->consecutive_failures = 0;
    sm->cooldown_until_ms = 0;
    sm->stop_completed = false;
    sm->start_completed = false;
    sm->got_ip = false;
    sm->mqtt_verified = false;
}

static bool in_boot_grace(const sp_conn_s3_input_t *in)
{
    return in->supervisor_up_ms < (int64_t)SP_CONN_STAGE3_BOOT_GRACE_MS;
}

static bool escalate_due(const sp_conn_s3_input_t *in)
{
    int64_t since;
    if (in_boot_grace(in)) {
        return false;
    }
    if (in->last_verified_ms <= 0) {
        since = in->supervisor_up_ms;
    } else {
        since = in->now_ms - in->last_verified_ms;
    }
    /*
     * Temporary disconnect, DHCP wait, and MQTT backoff are all shorter
     * than ESCALATE_MS; they cannot trip Stage 3 by themselves.
     */
    return since >= (int64_t)SP_CONN_STAGE3_ESCALATE_MS;
}

sp_conn_s3_eval_t sp_conn_s3_evaluate(sp_conn_s3_sm_t *sm, const sp_conn_s3_input_t *in)
{
    sp_conn_s3_eval_t out = {
        .state = SP_CONN_S3_IDLE,
        .phase = SP_CONN_S3_PHASE_NONE,
        .action = SP_CONN_S3_ACTION_NONE,
        .last_result = SP_CONN_S3_RESULT_NONE,
        .iface_generation = 0,
    };
    if (sm == NULL || in == NULL) {
        return out;
    }

    if (sm->state == SP_CONN_S3_ACTIVE) {
        int64_t phase_age = in->now_ms - sm->phase_started_ms;
        int64_t op_age = in->now_ms - sm->op_started_ms;
        if (sm->phase == SP_CONN_S3_PHASE_WAIT_MQTT
            && phase_age >= (int64_t)SP_CONN_STAGE3_MQTT_WAIT_MS) {
            finish_failure(sm, in->now_ms, SP_CONN_S3_RESULT_MQTT_TIMEOUT);
        } else if (sm->phase == SP_CONN_S3_PHASE_WAIT_IP
                   && phase_age >= (int64_t)SP_CONN_STAGE3_OP_TIMEOUT_MS) {
            finish_failure(sm, in->now_ms, SP_CONN_S3_RESULT_DHCP_TIMEOUT);
        } else if (sm->phase != SP_CONN_S3_PHASE_WAIT_IP
                   && sm->phase != SP_CONN_S3_PHASE_WAIT_MQTT
                   && op_age >= (int64_t)SP_CONN_STAGE3_OP_TIMEOUT_MS) {
            finish_failure(sm, in->now_ms, SP_CONN_S3_RESULT_OP_TIMEOUT);
        }
        out.state = sm->state;
        out.phase = sm->phase;
        out.last_result = sm->last_result;
        out.iface_generation = sm->iface_generation;
        out.action = SP_CONN_S3_ACTION_NONE;
        return out;
    }

    if (sm->state == SP_CONN_S3_EXHAUSTED) {
        out.state = sm->state;
        out.last_result = sm->last_result;
        out.iface_generation = sm->iface_generation;
        return out;
    }

    if (sm->state == SP_CONN_S3_COOLDOWN) {
        if (in->now_ms < sm->cooldown_until_ms) {
            out.state = sm->state;
            out.last_result = sm->last_result;
            out.iface_generation = sm->iface_generation;
            return out;
        }
        sm->state = SP_CONN_S3_IDLE;
    }

    if (sm->state == SP_CONN_S3_BLOCKED_BY_WORKER) {
        if (escalate_due(in) && in->worker_responsive) {
            sm->state = SP_CONN_S3_IDLE;
        } else {
            out.state = sm->state;
            out.last_result = SP_CONN_S3_RESULT_BLOCKED;
            out.iface_generation = sm->iface_generation;
            if (escalate_due(in) && !in->worker_responsive) {
                out.action = SP_CONN_S3_ACTION_RECORD_BLOCKED;
            }
            return out;
        }
    }

    out.state = sm->state;
    out.last_result = sm->last_result;
    out.iface_generation = sm->iface_generation;

    if (!escalate_due(in)) {
        return out;
    }

    if (!in->worker_responsive) {
        sm->state = SP_CONN_S3_BLOCKED_BY_WORKER;
        sm->last_result = SP_CONN_S3_RESULT_BLOCKED;
        out.state = sm->state;
        out.last_result = sm->last_result;
        out.action = SP_CONN_S3_ACTION_RECORD_BLOCKED;
        return out;
    }

    out.action = SP_CONN_S3_ACTION_BEGIN_IFACE_RESTART;
    return out;
}

void sp_conn_s3_note_begin(sp_conn_s3_sm_t *sm, int64_t now_ms)
{
    if (sm == NULL) {
        return;
    }
    if (sm->state == SP_CONN_S3_ACTIVE) {
        sm->last_result = SP_CONN_S3_RESULT_BUSY;
        return;
    }
    if (sm->iface_generation < UINT32_MAX) {
        sm->iface_generation++;
    } else {
        sm->iface_generation = 1;
    }
    if (sm->attempt_count < UINT32_MAX) {
        sm->attempt_count++;
    }
    sm->state = SP_CONN_S3_ACTIVE;
    sm->phase = SP_CONN_S3_PHASE_STOP;
    sm->op_started_ms = now_ms;
    sm->phase_started_ms = now_ms;
    sm->stop_completed = false;
    sm->start_completed = false;
    sm->got_ip = false;
    sm->mqtt_verified = false;
    sm->last_result = SP_CONN_S3_RESULT_NONE;
}

void sp_conn_s3_on_stop_ok(sp_conn_s3_sm_t *sm, int64_t now_ms)
{
    if (sm == NULL || sm->state != SP_CONN_S3_ACTIVE) {
        return;
    }
    if (sm->phase != SP_CONN_S3_PHASE_STOP) {
        return;
    }
    sm->stop_completed = true;
    sm->phase = SP_CONN_S3_PHASE_START;
    sm->phase_started_ms = now_ms;
}

void sp_conn_s3_on_stop_fail(sp_conn_s3_sm_t *sm, int64_t now_ms)
{
    if (sm == NULL || sm->state != SP_CONN_S3_ACTIVE) {
        return;
    }
    finish_failure(sm, now_ms, SP_CONN_S3_RESULT_STOP_FAIL);
}

void sp_conn_s3_on_start_ok(sp_conn_s3_sm_t *sm, int64_t now_ms)
{
    if (sm == NULL || sm->state != SP_CONN_S3_ACTIVE) {
        return;
    }
    if (sm->phase != SP_CONN_S3_PHASE_START) {
        return;
    }
    sm->start_completed = true;
    sm->phase = SP_CONN_S3_PHASE_CONNECT;
    sm->phase_started_ms = now_ms;
}

void sp_conn_s3_on_start_fail(sp_conn_s3_sm_t *sm, int64_t now_ms)
{
    if (sm == NULL || sm->state != SP_CONN_S3_ACTIVE) {
        return;
    }
    finish_failure(sm, now_ms, SP_CONN_S3_RESULT_START_FAIL);
}

void sp_conn_s3_on_connect_started(sp_conn_s3_sm_t *sm, int64_t now_ms)
{
    if (sm == NULL || sm->state != SP_CONN_S3_ACTIVE) {
        return;
    }
    if (sm->phase != SP_CONN_S3_PHASE_CONNECT
        && sm->phase != SP_CONN_S3_PHASE_START) {
        return;
    }
    sm->phase = SP_CONN_S3_PHASE_WAIT_IP;
    sm->phase_started_ms = now_ms;
}

bool sp_conn_s3_on_got_ip(
    sp_conn_s3_sm_t *sm,
    uint32_t iface_generation,
    int64_t now_ms
)
{
    if (sm == NULL || sm->state != SP_CONN_S3_ACTIVE) {
        return false;
    }
    if (iface_generation != sm->iface_generation) {
        return false;
    }
    if (sm->phase != SP_CONN_S3_PHASE_WAIT_IP
        && sm->phase != SP_CONN_S3_PHASE_CONNECT) {
        return false;
    }
    sm->got_ip = true;
    sm->phase = SP_CONN_S3_PHASE_WAIT_MQTT;
    sm->phase_started_ms = now_ms;
    return true;
}

void sp_conn_s3_on_mqtt_verified(sp_conn_s3_sm_t *sm, int64_t now_ms)
{
    if (sm == NULL) {
        return;
    }
    if (sm->state == SP_CONN_S3_ACTIVE
        && sm->phase == SP_CONN_S3_PHASE_WAIT_MQTT) {
        sm->mqtt_verified = true;
        finish_success(sm, now_ms);
        return;
    }
    sp_conn_s3_on_verified_progress_idle(sm);
}

void sp_conn_s3_on_verified_progress_idle(sp_conn_s3_sm_t *sm)
{
    if (sm == NULL) {
        return;
    }
    if (sm->state == SP_CONN_S3_ACTIVE) {
        return;
    }
    sm->consecutive_failures = 0;
    if (sm->state == SP_CONN_S3_EXHAUSTED || sm->state == SP_CONN_S3_COOLDOWN
        || sm->state == SP_CONN_S3_BLOCKED_BY_WORKER) {
        sm->state = SP_CONN_S3_IDLE;
        sm->cooldown_until_ms = 0;
    }
}
