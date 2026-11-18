/**
 * Host tests for Stage 4 controlled reboot (no Observation END).
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *     -I firmware/platform/node_common/include \
 *     firmware/platform/node_common/connectivity_stage4_sm.c \
 *     firmware/platform/node_common/test_host/test_connectivity_stage4_sm.c \
 *     -o /tmp/test_stage4 && /tmp/test_stage4
 */
#include "sp_connectivity_stage4.h"

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

static sp_conn_s4_input_t in_at(int64_t now, int64_t last_v)
{
    sp_conn_s4_input_t in;
    memset(&in, 0, sizeof(in));
    in.now_ms = now;
    in.last_verified_ms = last_v;
    return in;
}

static void test_thresholds(void)
{
    EXPECT(SP_CONN_STAGE4_NO_PROGRESS_MS == 60u * 60u * 1000u);
    EXPECT(SP_CONN_STAGE4_REBOOT_CLEANUP_MS == 500u);
}

static void test_no_reboot_before_60(void)
{
    sp_conn_s4_sm_t sm;
    sp_conn_s4_sm_init(&sm);
    sp_conn_s4_input_t in = in_at(SP_CONN_STAGE4_NO_PROGRESS_MS - 1, 0);
    sp_conn_s4_eval_t r = sp_conn_s4_evaluate(&sm, &in);
    EXPECT(r.action == SP_CONN_S4_ACTION_NONE);
    EXPECT(sm.state == SP_CONN_S4_IDLE);
}

static void test_reboot_at_60_never_verified(void)
{
    sp_conn_s4_sm_t sm;
    sp_conn_s4_sm_init(&sm);
    sp_conn_s4_input_t in = in_at(SP_CONN_STAGE4_NO_PROGRESS_MS, 0);
    sp_conn_s4_eval_t r = sp_conn_s4_evaluate(&sm, &in);
    EXPECT(r.action == SP_CONN_S4_ACTION_EXECUTE_REBOOT);
    EXPECT(sm.state == SP_CONN_S4_REBOOT_PENDING);
}

static void test_progress_resets_timer(void)
{
    sp_conn_s4_sm_t sm;
    sp_conn_s4_sm_init(&sm);
    /* Progress at 50 min; now at 70 min → only 20 min since verify. */
    int64_t verified = 50LL * 60LL * 1000LL;
    int64_t now = 70LL * 60LL * 1000LL;
    sp_conn_s4_input_t in = in_at(now, verified);
    EXPECT(sp_conn_s4_evaluate(&sm, &in).action == SP_CONN_S4_ACTION_NONE);

    /* Progress before threshold keeps Stage 4 idle. */
    sp_conn_s4_sm_init(&sm);
    in = in_at(SP_CONN_STAGE4_NO_PROGRESS_MS - 1000, 0);
    EXPECT(sp_conn_s4_evaluate(&sm, &in).action == SP_CONN_S4_ACTION_NONE);
    sp_conn_s4_on_mqtt_verified(&sm, SP_CONN_STAGE4_NO_PROGRESS_MS - 500);
    in = in_at(SP_CONN_STAGE4_NO_PROGRESS_MS - 500, SP_CONN_STAGE4_NO_PROGRESS_MS - 500);
    EXPECT(sp_conn_s4_evaluate(&sm, &in).action == SP_CONN_S4_ACTION_NONE);
}

static void test_outage_then_restore_resets(void)
{
    sp_conn_s4_sm_t sm;
    sp_conn_s4_sm_init(&sm);
    int64_t verified = 10LL * 60LL * 1000LL;
    sp_conn_s4_input_t in =
        in_at(verified + SP_CONN_STAGE4_NO_PROGRESS_MS - 1, verified);
    EXPECT(sp_conn_s4_evaluate(&sm, &in).action == SP_CONN_S4_ACTION_NONE);

    /* Progress restores before threshold. */
    int64_t restored = verified + SP_CONN_STAGE4_NO_PROGRESS_MS - 500;
    sp_conn_s4_on_mqtt_verified(&sm, restored);
    in = in_at(restored, restored);
    EXPECT(sp_conn_s4_evaluate(&sm, &in).action == SP_CONN_S4_ACTION_NONE);

    /* New 60-minute window from restored. */
    in = in_at(restored + SP_CONN_STAGE4_NO_PROGRESS_MS - 1, restored);
    EXPECT(sp_conn_s4_evaluate(&sm, &in).action == SP_CONN_S4_ACTION_NONE);
    in = in_at(restored + SP_CONN_STAGE4_NO_PROGRESS_MS, restored);
    EXPECT(sp_conn_s4_evaluate(&sm, &in).action == SP_CONN_S4_ACTION_EXECUTE_REBOOT);
}

static void test_direct_reboot_no_obs_end_api(void)
{
    /*
     * This TU only links connectivity_stage4_sm.c — no sp_ota / observation
     * symbols. Eligibility must yield EXECUTE_REBOOT immediately (no close
     * action or close APIs in the Stage 4 public header).
     */
    sp_conn_s4_sm_t sm;
    sp_conn_s4_sm_init(&sm);
    sp_conn_s4_input_t in = in_at(SP_CONN_STAGE4_NO_PROGRESS_MS, 0);
    sp_conn_s4_eval_t r = sp_conn_s4_evaluate(&sm, &in);
    EXPECT(r.action == SP_CONN_S4_ACTION_EXECUTE_REBOOT);
    /* Compile-time: BEGIN_OBS_CLOSE / OBS_CLOSING / note_close_* must not exist. */
#if defined(SP_CONN_STAGE4_OBS_CLOSE_BUDGET_MS)
#error "Stage 4 must not expose Observation-close budget"
#endif
#if defined(SP_CONN_S4_ACTION_BEGIN_OBS_CLOSE) || defined(SP_CONN_S4_OBS_CLOSING)
#error "Stage 4 must not expose Observation-close actions/states"
#endif
}

static void test_no_nvs_budget(void)
{
    sp_conn_s4_sm_t sm;
    sp_conn_s4_sm_init(&sm);
    int64_t t = SP_CONN_STAGE4_NO_PROGRESS_MS;
    sp_conn_s4_input_t in = in_at(t, 0);
    EXPECT(sp_conn_s4_evaluate(&sm, &in).action == SP_CONN_S4_ACTION_EXECUTE_REBOOT);
    /* Fresh SM after a hypothetical reboot: another 60 min window. */
    sp_conn_s4_sm_init(&sm);
    in = in_at(t - 1, 0);
    EXPECT(sp_conn_s4_evaluate(&sm, &in).action == SP_CONN_S4_ACTION_NONE);
    in = in_at(t, 0);
    EXPECT(sp_conn_s4_evaluate(&sm, &in).action == SP_CONN_S4_ACTION_EXECUTE_REBOOT);
}

static void test_ota_pending_suppresses(void)
{
    sp_conn_s4_sm_t sm;
    sp_conn_s4_sm_init(&sm);
    sp_conn_s4_input_t in = in_at(SP_CONN_STAGE4_NO_PROGRESS_MS, 0);
    in.reboot_pending_other = true;
    EXPECT(sp_conn_s4_evaluate(&sm, &in).action == SP_CONN_S4_ACTION_NONE);
    EXPECT(sm.state == SP_CONN_S4_IDLE);
}

static void test_overflow_safe_age(void)
{
    sp_conn_s4_input_t in = in_at(1000, 2000);
    EXPECT(sp_conn_s4_no_progress_age_ms(&in) == 0);
}

int main(void)
{
    test_thresholds();
    test_no_reboot_before_60();
    test_reboot_at_60_never_verified();
    test_progress_resets_timer();
    test_outage_then_restore_resets();
    test_direct_reboot_no_obs_end_api();
    test_no_nvs_budget();
    test_ota_pending_suppresses();
    test_overflow_safe_age();

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
