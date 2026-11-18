#include "sp_connectivity_stage4.h"

#include <string.h>

void sp_conn_s4_sm_init(sp_conn_s4_sm_t *sm)
{
    if (sm == NULL) {
        return;
    }
    memset(sm, 0, sizeof(*sm));
    sm->state = SP_CONN_S4_IDLE;
}

int64_t sp_conn_s4_no_progress_age_ms(const sp_conn_s4_input_t *in)
{
    if (in == NULL) {
        return 0;
    }
    if (in->last_verified_ms <= 0) {
        /* esp_timer monotonic origin is boot; now_ms is uptime. */
        return in->now_ms > 0 ? in->now_ms : 0;
    }
    if (in->now_ms < in->last_verified_ms) {
        return 0;
    }
    return in->now_ms - in->last_verified_ms;
}

sp_conn_s4_eval_t sp_conn_s4_evaluate(sp_conn_s4_sm_t *sm, const sp_conn_s4_input_t *in)
{
    sp_conn_s4_eval_t out;
    memset(&out, 0, sizeof(out));
    out.action = SP_CONN_S4_ACTION_NONE;
    if (sm == NULL || in == NULL) {
        return out;
    }

    const int64_t age = sp_conn_s4_no_progress_age_ms(in);
    out.no_progress_age_ms = age;
    out.state = sm->state;

    if (in->reboot_pending_other) {
        return out;
    }

    if (sm->state == SP_CONN_S4_REBOOT_PENDING) {
        out.action = SP_CONN_S4_ACTION_EXECUTE_REBOOT;
        return out;
    }

    if (age < (int64_t)SP_CONN_STAGE4_NO_PROGRESS_MS) {
        return out;
    }

    sm->state = SP_CONN_S4_REBOOT_PENDING;
    out.state = sm->state;
    out.action = SP_CONN_S4_ACTION_EXECUTE_REBOOT;
    return out;
}

void sp_conn_s4_on_mqtt_verified(sp_conn_s4_sm_t *sm, int64_t now_ms)
{
    if (sm == NULL) {
        return;
    }
    (void)now_ms;
    if (sm->state == SP_CONN_S4_REBOOT_PENDING) {
        return; /* reboot already armed */
    }
    memset(sm, 0, sizeof(*sm));
    sm->state = SP_CONN_S4_IDLE;
}
