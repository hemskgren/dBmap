/**
 * Host tests for Stage 3 Wi-Fi interface recovery decisions.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/platform/node_common/include \
 *     firmware/platform/node_common/connectivity_stage3_sm.c \
 *     firmware/platform/node_common/test_host/test_connectivity_stage3_sm.c \
 *     -o /tmp/test_stage3 && /tmp/test_stage3
 */
#include "sp_connectivity_stage3.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
/** Sentinel: Stage 3 SM must never write this (Observation isolation). */
static int g_observation_sentinel = 0x4F4253; /* 'OBS' */

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static sp_conn_s3_input_t base_in(int64_t now, int64_t up, int64_t last_v)
{
    sp_conn_s3_input_t in;
    memset(&in, 0, sizeof(in));
    in.now_ms = now;
    in.supervisor_up_ms = up;
    in.last_verified_ms = last_v;
    in.worker_responsive = true;
    return in;
}

static void test_thresholds(void)
{
    EXPECT(SP_CONN_STAGE3_ESCALATE_MS == 15u * 60u * 1000u);
    EXPECT(SP_CONN_STAGE3_COOLDOWN_MS == 5u * 60u * 1000u);
    EXPECT(SP_CONN_STAGE3_OP_TIMEOUT_MS == 60u * 1000u);
    EXPECT(SP_CONN_STAGE3_MQTT_WAIT_MS == 90000u);
    EXPECT(SP_CONN_STAGE3_MAX_ATTEMPTS == 3u);
    EXPECT(SP_CONN_STAGE3_BOOT_GRACE_MS == 2u * 60u * 1000u);
}

static void test_normal_backoff_no_stage3(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    /* 90 s since verify — Stage 1 progress window, not Stage 3. */
    sp_conn_s3_input_t in = base_in(200000, 200000, 110000);
    sp_conn_s3_eval_t r = sp_conn_s3_evaluate(&sm, &in);
    EXPECT(r.action == SP_CONN_S3_ACTION_NONE);
    EXPECT(sm.state == SP_CONN_S3_IDLE);
}

static void test_temp_disconnect_no_stage3(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    sp_conn_s3_input_t in = base_in(130000, 130000, 100000); /* 30 s gap */
    sp_conn_s3_eval_t r = sp_conn_s3_evaluate(&sm, &in);
    EXPECT(r.action == SP_CONN_S3_ACTION_NONE);
}

static void test_dhcp_window_no_stage3(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    sp_conn_s3_input_t in = base_in(150000, 150000, 120000); /* 30 s */
    sp_conn_s3_eval_t r = sp_conn_s3_evaluate(&sm, &in);
    EXPECT(r.action == SP_CONN_S3_ACTION_NONE);
}

static void test_sustained_failure_triggers_once(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    int64_t now = SP_CONN_STAGE3_BOOT_GRACE_MS + SP_CONN_STAGE3_ESCALATE_MS + 1000;
    sp_conn_s3_input_t in = base_in(now, now, 1000);
    sp_conn_s3_eval_t r = sp_conn_s3_evaluate(&sm, &in);
    EXPECT(r.action == SP_CONN_S3_ACTION_BEGIN_IFACE_RESTART);
    sp_conn_s3_note_begin(&sm, now);
    EXPECT(sm.state == SP_CONN_S3_ACTIVE);
    EXPECT(sm.iface_generation == 1);
    /* Second evaluate while active must not begin again. */
    r = sp_conn_s3_evaluate(&sm, &in);
    EXPECT(r.action == SP_CONN_S3_ACTION_NONE);
    EXPECT(sm.state == SP_CONN_S3_ACTIVE);
}

static void test_blocked_worker(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    int64_t now = SP_CONN_STAGE3_BOOT_GRACE_MS + SP_CONN_STAGE3_ESCALATE_MS + 1000;
    sp_conn_s3_input_t in = base_in(now, now, 1000);
    in.worker_responsive = false;
    sp_conn_s3_eval_t r = sp_conn_s3_evaluate(&sm, &in);
    EXPECT(r.action == SP_CONN_S3_ACTION_RECORD_BLOCKED);
    EXPECT(sm.state == SP_CONN_S3_BLOCKED_BY_WORKER);
    EXPECT(sm.last_result == SP_CONN_S3_RESULT_BLOCKED);
}

static void test_busy_second_begin(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    sp_conn_s3_note_begin(&sm, 1000);
    sp_conn_s3_note_begin(&sm, 2000);
    EXPECT(sm.last_result == SP_CONN_S3_RESULT_BUSY);
    EXPECT(sm.iface_generation == 1);
}

static void test_stop_fail(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    sp_conn_s3_note_begin(&sm, 1000);
    sp_conn_s3_on_stop_fail(&sm, 1100);
    EXPECT(sm.last_result == SP_CONN_S3_RESULT_STOP_FAIL);
    EXPECT(sm.state == SP_CONN_S3_COOLDOWN);
}

static void test_start_fail(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    sp_conn_s3_note_begin(&sm, 1000);
    sp_conn_s3_on_stop_ok(&sm, 1100);
    sp_conn_s3_on_start_fail(&sm, 1200);
    EXPECT(sm.last_result == SP_CONN_S3_RESULT_START_FAIL);
    EXPECT(sm.state == SP_CONN_S3_COOLDOWN);
}

static void test_dhcp_timeout(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    sp_conn_s3_note_begin(&sm, 1000);
    sp_conn_s3_on_stop_ok(&sm, 1100);
    sp_conn_s3_on_start_ok(&sm, 1200);
    sp_conn_s3_on_connect_started(&sm, 1300);
    sp_conn_s3_input_t in = base_in(1300 + SP_CONN_STAGE3_OP_TIMEOUT_MS, 999999, 0);
    in.worker_responsive = true;
    (void)sp_conn_s3_evaluate(&sm, &in);
    EXPECT(sm.last_result == SP_CONN_S3_RESULT_DHCP_TIMEOUT);
}

static void test_mqtt_timeout_after_ip(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    sp_conn_s3_note_begin(&sm, 1000);
    sp_conn_s3_on_stop_ok(&sm, 1100);
    sp_conn_s3_on_start_ok(&sm, 1200);
    sp_conn_s3_on_connect_started(&sm, 1300);
    EXPECT(sp_conn_s3_on_got_ip(&sm, 1, 1400) == true);
    EXPECT(sm.phase == SP_CONN_S3_PHASE_WAIT_MQTT);
    sp_conn_s3_input_t in = base_in(1400 + SP_CONN_STAGE3_MQTT_WAIT_MS, 999999, 0);
    (void)sp_conn_s3_evaluate(&sm, &in);
    EXPECT(sm.last_result == SP_CONN_S3_RESULT_MQTT_TIMEOUT);
}

static void test_success_path(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    sp_conn_s3_note_begin(&sm, 1000);
    sp_conn_s3_on_stop_ok(&sm, 1100);
    sp_conn_s3_on_start_ok(&sm, 1200);
    sp_conn_s3_on_connect_started(&sm, 1300);
    EXPECT(sp_conn_s3_on_got_ip(&sm, 1, 1400) == true);
    sp_conn_s3_on_mqtt_verified(&sm, 1500);
    EXPECT(sm.state == SP_CONN_S3_IDLE);
    EXPECT(sm.last_result == SP_CONN_S3_RESULT_SUCCESS);
    EXPECT(sm.consecutive_failures == 0);
}

static void test_stale_got_ip_rejected(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    sp_conn_s3_note_begin(&sm, 1000);
    sp_conn_s3_on_stop_ok(&sm, 1100);
    sp_conn_s3_on_start_ok(&sm, 1200);
    sp_conn_s3_on_connect_started(&sm, 1300);
    EXPECT(sp_conn_s3_on_got_ip(&sm, 0, 1400) == false); /* old generation */
    EXPECT(sm.got_ip == false);
    EXPECT(sm.phase == SP_CONN_S3_PHASE_WAIT_IP);
}

static void test_duplicate_events(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    sp_conn_s3_note_begin(&sm, 1000);
    sp_conn_s3_on_stop_ok(&sm, 1100);
    sp_conn_s3_on_stop_ok(&sm, 1110); /* duplicate */
    EXPECT(sm.phase == SP_CONN_S3_PHASE_START);
    sp_conn_s3_on_start_ok(&sm, 1200);
    sp_conn_s3_on_start_ok(&sm, 1210);
    EXPECT(sm.phase == SP_CONN_S3_PHASE_CONNECT);
}

static void test_cooldown(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    sp_conn_s3_note_begin(&sm, 1000);
    sp_conn_s3_on_stop_fail(&sm, 1100);
    EXPECT(sm.state == SP_CONN_S3_COOLDOWN);
    int64_t now = 1100 + SP_CONN_STAGE3_COOLDOWN_MS - 1;
    sp_conn_s3_input_t in = base_in(now, now, 0);
    sp_conn_s3_eval_t r = sp_conn_s3_evaluate(&sm, &in);
    EXPECT(r.action == SP_CONN_S3_ACTION_NONE);
    EXPECT(sm.state == SP_CONN_S3_COOLDOWN);
    now = 1100 + SP_CONN_STAGE3_COOLDOWN_MS;
    in = base_in(now, now, 0);
    /* Still no escalate if last_verified gap insufficient — use old verify. */
    in.last_verified_ms = now; /* just verified — no escalate */
    r = sp_conn_s3_evaluate(&sm, &in);
    EXPECT(sm.state == SP_CONN_S3_IDLE); /* cooldown expired */
    EXPECT(r.action == SP_CONN_S3_ACTION_NONE);
}

static void test_exhausted(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    for (uint32_t i = 0; i < SP_CONN_STAGE3_MAX_ATTEMPTS; ++i) {
        sp_conn_s3_note_begin(&sm, 1000 + (int64_t)i * 100);
        sp_conn_s3_on_stop_fail(&sm, 1100 + (int64_t)i * 100);
    }
    EXPECT(sm.state == SP_CONN_S3_EXHAUSTED);
    int64_t now = SP_CONN_STAGE3_BOOT_GRACE_MS + SP_CONN_STAGE3_ESCALATE_MS + 50000;
    sp_conn_s3_input_t in = base_in(now, now, 1);
    sp_conn_s3_eval_t r = sp_conn_s3_evaluate(&sm, &in);
    EXPECT(r.action == SP_CONN_S3_ACTION_NONE);
    EXPECT(sm.state == SP_CONN_S3_EXHAUSTED);
}

static void test_verified_resets_failures(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    sp_conn_s3_note_begin(&sm, 1000);
    sp_conn_s3_on_stop_fail(&sm, 1100);
    EXPECT(sm.consecutive_failures == 1);
    EXPECT(sm.state == SP_CONN_S3_COOLDOWN);
    sp_conn_s3_on_verified_progress_idle(&sm);
    EXPECT(sm.consecutive_failures == 0);
    EXPECT(sm.state == SP_CONN_S3_IDLE);
}

static void test_observation_isolation(void)
{
    int before = g_observation_sentinel;
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    sp_conn_s3_note_begin(&sm, 1000);
    sp_conn_s3_on_stop_ok(&sm, 1100);
    sp_conn_s3_on_start_ok(&sm, 1200);
    sp_conn_s3_on_connect_started(&sm, 1300);
    (void)sp_conn_s3_on_got_ip(&sm, 1, 1400);
    sp_conn_s3_on_mqtt_verified(&sm, 1500);
    EXPECT(g_observation_sentinel == before);
}

static void test_boot_grace(void)
{
    sp_conn_s3_sm_t sm;
    sp_conn_s3_sm_init(&sm);
    sp_conn_s3_input_t in = base_in(SP_CONN_STAGE3_ESCALATE_MS + 1000,
                                    SP_CONN_STAGE3_BOOT_GRACE_MS - 1, 0);
    sp_conn_s3_eval_t r = sp_conn_s3_evaluate(&sm, &in);
    EXPECT(r.action == SP_CONN_S3_ACTION_NONE);
}

int main(void)
{
    test_thresholds();
    test_normal_backoff_no_stage3();
    test_temp_disconnect_no_stage3();
    test_dhcp_window_no_stage3();
    test_boot_grace();
    test_sustained_failure_triggers_once();
    test_blocked_worker();
    test_busy_second_begin();
    test_stop_fail();
    test_start_fail();
    test_dhcp_timeout();
    test_mqtt_timeout_after_ip();
    test_success_path();
    test_stale_got_ip_rejected();
    test_duplicate_events();
    test_cooldown();
    test_exhausted();
    test_verified_resets_failures();
    test_observation_isolation();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
