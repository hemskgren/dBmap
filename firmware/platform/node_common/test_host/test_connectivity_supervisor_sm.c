/**
 * Host tests for Stage 2 connectivity supervisor decisions.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/platform/node_common/include \
 *     firmware/platform/node_common/connectivity_supervisor_sm.c \
 *     firmware/platform/node_common/test_host/test_connectivity_supervisor_sm.c \
 *     -o /tmp/test_conn_sm && /tmp/test_conn_sm
 */
#include "sp_connectivity.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static sp_conn_sm_input_t base_ok(int64_t now)
{
    sp_conn_sm_input_t in;
    memset(&in, 0, sizeof(in));
    in.now_ms = now;
    in.wifi_has_ip = true;
    in.mqtt_connected = true;
    in.mqtt_progress_expired = false;
    in.mqtt_progress_age_ms = 1000;
    in.worker_started = true;
    in.supervisor_started = true;
    in.dispatch_pending = false;
    in.worker_heartbeat_age_ms = 100;
    in.last_dispatch_age_ms = 500;
    in.pending_age_ms = -1;
    return in;
}

static void test_healthy_session(void)
{
    sp_conn_sm_t sm;
    sp_conn_sm_init(&sm);
    sp_conn_sm_input_t in = base_ok(10000);
    sp_conn_sm_result_t r = sp_conn_sm_evaluate(&sm, &in);
    EXPECT(r.stage == SP_CONN_STAGE_OK);
    EXPECT(r.action == SP_CONN_ACTION_NONE);
}

static void test_mqtt_backoff_not_stall(void)
{
    sp_conn_sm_t sm;
    sp_conn_sm_init(&sm);
    sp_conn_sm_input_t in = base_ok(20000);
    in.mqtt_connected = false;
    in.mqtt_backoff_waiting = true;
    in.mqtt_next_attempt_in_ms = 45000;
    in.worker_heartbeat_age_ms = 60000; /* idle wait may look old */
    in.dispatch_pending = false;
    sp_conn_sm_result_t r = sp_conn_sm_evaluate(&sm, &in);
    EXPECT(r.stage == SP_CONN_STAGE_MQTT_BACKOFF);
    EXPECT(r.action == SP_CONN_ACTION_NONE);
}

static void test_mqtt_no_progress_requests_recovery(void)
{
    sp_conn_sm_t sm;
    sp_conn_sm_init(&sm);
    sp_conn_sm_input_t in = base_ok(90000);
    in.mqtt_progress_expired = true;
    in.mqtt_progress_age_ms = 90000;
    sp_conn_sm_result_t r = sp_conn_sm_evaluate(&sm, &in);
    EXPECT(r.stage == SP_CONN_STAGE_MQTT_NO_PROGRESS);
    EXPECT(r.action == SP_CONN_ACTION_REQUEST_MQTT_RECOVERY);
}

static void test_recovery_already_active(void)
{
    sp_conn_sm_t sm;
    sp_conn_sm_init(&sm);
    sp_conn_sm_note_recovery_started(&sm);
    sp_conn_sm_input_t in = base_ok(90000);
    in.mqtt_progress_expired = true;
    in.recovery_request_active = true;
    sp_conn_sm_result_t r = sp_conn_sm_evaluate(&sm, &in);
    EXPECT(r.stage == SP_CONN_STAGE_MQTT_NO_PROGRESS);
    EXPECT(r.action == SP_CONN_ACTION_NONE);

    /* Still expired on the next tick must not spam another request. */
    r = sp_conn_sm_evaluate(&sm, &in);
    EXPECT(r.action == SP_CONN_ACTION_NONE);
}

static void test_worker_heartbeat_stopped(void)
{
    sp_conn_sm_t sm;
    sp_conn_sm_init(&sm);
    sp_conn_sm_input_t in = base_ok(50000);
    in.dispatch_pending = true;
    in.pending_age_ms = 35000;
    in.worker_heartbeat_age_ms = (int64_t)SP_CONN_WORKER_STALL_MS;
    sp_conn_sm_result_t r = sp_conn_sm_evaluate(&sm, &in);
    EXPECT(r.stage == SP_CONN_STAGE_WORKER_STALLED);
    EXPECT(r.action == SP_CONN_ACTION_RECORD_UNRECOVERABLE_STALL);
}

static void test_dispatch_not_processed(void)
{
    sp_conn_sm_t sm;
    sp_conn_sm_init(&sm);
    sp_conn_sm_input_t in = base_ok(50000);
    in.dispatch_pending = true;
    in.worker_heartbeat_age_ms = 500; /* worker looping */
    in.pending_age_ms = (int64_t)SP_CONN_DISPATCH_STALL_MS;
    in.last_dispatch_age_ms = (int64_t)SP_CONN_DISPATCH_STALL_MS + 1000;
    sp_conn_sm_result_t r = sp_conn_sm_evaluate(&sm, &in);
    EXPECT(r.stage == SP_CONN_STAGE_DISPATCH_STALLED);
    EXPECT(r.action == SP_CONN_ACTION_RECORD_UNRECOVERABLE_STALL);
}

static void test_timer_arm_failure_bounded_retry(void)
{
    sp_conn_sm_t sm;
    sp_conn_sm_init(&sm);
    sm.last_fail = SP_CONN_FAIL_MQTT_PROGRESS_TIMER;
    sp_conn_sm_input_t in = base_ok(1000);
    in.timer_arm_failed = true;
    in.timer_arm_retries = 0;
    sp_conn_sm_result_t r = sp_conn_sm_evaluate(&sm, &in);
    EXPECT(r.stage == SP_CONN_STAGE_TIMER_ARM_FAULT);
    EXPECT(r.action == SP_CONN_ACTION_RETRY_TIMER_PATH);

    in.timer_arm_retries = SP_CONN_TIMER_RETRY_MAX;
    r = sp_conn_sm_evaluate(&sm, &in);
    EXPECT(r.stage == SP_CONN_STAGE_TIMER_ARM_FAULT);
    EXPECT(r.action == SP_CONN_ACTION_NONE);
}

static void test_supervisor_init_failure(void)
{
    sp_conn_sm_t sm;
    sp_conn_sm_init(&sm);
    sp_conn_sm_input_t in = base_ok(0);
    in.supervisor_started = false;
    sp_conn_sm_result_t r = sp_conn_sm_evaluate(&sm, &in);
    EXPECT(r.stage == SP_CONN_STAGE_INIT_FAILED);
    EXPECT(r.fail_reason == SP_CONN_FAIL_SUPERVISOR_TASK);

    in.supervisor_started = true;
    in.worker_started = false;
    r = sp_conn_sm_evaluate(&sm, &in);
    EXPECT(r.stage == SP_CONN_STAGE_INIT_FAILED);
    EXPECT(r.fail_reason == SP_CONN_FAIL_WORKER_TASK);
}

static void test_progress_resets_to_ok(void)
{
    sp_conn_sm_t sm;
    sp_conn_sm_init(&sm);
    sp_conn_sm_input_t bad = base_ok(90000);
    bad.mqtt_progress_expired = true;
    (void)sp_conn_sm_evaluate(&sm, &bad);
    EXPECT(sm.stage == SP_CONN_STAGE_MQTT_NO_PROGRESS);

    sp_conn_sm_input_t good = base_ok(100000);
    sp_conn_sm_result_t r = sp_conn_sm_evaluate(&sm, &good);
    EXPECT(r.stage == SP_CONN_STAGE_OK);
    EXPECT(sm.stage == SP_CONN_STAGE_OK);
}

static void test_no_ip(void)
{
    sp_conn_sm_t sm;
    sp_conn_sm_init(&sm);
    sp_conn_sm_input_t in = base_ok(1000);
    in.wifi_has_ip = false;
    sp_conn_sm_result_t r = sp_conn_sm_evaluate(&sm, &in);
    EXPECT(r.stage == SP_CONN_STAGE_NO_IP);
    EXPECT(r.action == SP_CONN_ACTION_NONE);
}

static void test_thresholds_named(void)
{
    EXPECT(SP_CONN_PROGRESS_LIMIT_MS == 90000u);
    EXPECT(SP_CONN_WORKER_STALL_MS == 30000u);
    EXPECT(SP_CONN_DISPATCH_STALL_MS == 30000u);
    EXPECT(SP_CONN_SUPERVISOR_PERIOD_MS == 5000u);
    EXPECT(SP_CONN_TIMER_RETRY_MIN_MS == 1000u);
    EXPECT(SP_CONN_TIMER_RETRY_MAX == 5u);
}

int main(void)
{
    test_thresholds_named();
    test_healthy_session();
    test_mqtt_backoff_not_stall();
    test_mqtt_no_progress_requests_recovery();
    test_recovery_already_active();
    test_worker_heartbeat_stopped();
    test_dispatch_not_processed();
    test_timer_arm_failure_bounded_retry();
    test_supervisor_init_failure();
    test_progress_resets_to_ok();
    test_no_ip();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
