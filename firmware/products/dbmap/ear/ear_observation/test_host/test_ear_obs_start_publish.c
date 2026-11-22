/**
 * Host seam for production START preparation and the lifecycle section.
 *
 * cc -std=c11 -Wall -Wextra -Werror -O2 \
 *   -I firmware/products/dbmap/ear/ear_acoustic/include \
 *   -I firmware/products/dbmap/ear/ear_observation/include \
 *   -I firmware/platform/audio/include \
 *   -I firmware/platform/observation/include \
 *   ear_obs_start_retain.c ear_obs_lifecycle_section.c observation_lifecycle.c \
 *   test_ear_obs_start_publish.c -o /tmp/test_ear_obs_start_publish
 */
#include "ear_obs_lifecycle_section.h"
#include "ear_obs_start_retain.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static float g_ring_rms;
static int g_builds;

static void build_from_ring(
    void *ctx,
    int64_t interval_start_mono_us,
    int64_t capture_mono_us,
    ear_obs_start_aggregates_t *out
)
{
    (void)ctx;
    (void)capture_mono_us;
    g_builds++;
    memset(out, 0, sizeof(*out));
    out->l0.has_rms_stats = true;
    out->l0.rms_mean_dbfs = g_ring_rms;
    out->l0.window_count = 8u;
    out->have_l1 = true;
    out->l1.window_count = 8u;
    out->l1.interval_start_mono_us = interval_start_mono_us;
    out->have_l2 = true;
    out->l2.window_count = 5u;
    out->l2.analysis_window_ms = 1000u;
}

static void test_prepare_reuses_original_aggregate(void)
{
    ear_obs_start_retain_reset();
    g_builds = 0;
    g_ring_rms = -41.0f;

    const int64_t capture = 5000000;
    const int64_t binding_start = 1000000;
    const int64_t interval_us = 1000000;
    ear_obs_start_measure_t meas;
    bool built = false;
    EXPECT(ear_obs_start_prepare(
               "obs-start",
               4u,
               capture,
               binding_start,
               interval_us,
               build_from_ring,
               NULL,
               &meas,
               &built
           )
           == EAR_OBS_START_PREPARE_READY);
    EXPECT(built);
    EXPECT(g_builds == 1);
    EXPECT(meas.capture_mono_us == capture);
    EXPECT(meas.interval_start_mono_us == 4000000);
    EXPECT(meas.l0.rms_mean_dbfs == -41.0f);
    EXPECT(meas.l1.interval_start_mono_us == 4000000);
    EXPECT(meas.l2.window_count == 5u);

    /* First publication fails. The ring then moves. The retained aggregate stays. */
    g_ring_rms = 12.0f;
    ear_obs_start_measure_t again;
    built = true;
    EXPECT(ear_obs_start_prepare(
               "obs-start",
               4u,
               capture,
               binding_start,
               interval_us,
               build_from_ring,
               NULL,
               &again,
               &built
           )
           == EAR_OBS_START_PREPARE_READY);
    EXPECT(!built);
    EXPECT(g_builds == 1);
    EXPECT(again.capture_mono_us == capture);
    EXPECT(again.interval_start_mono_us == meas.interval_start_mono_us);
    EXPECT(again.l0.rms_mean_dbfs == -41.0f);
    EXPECT(again.l0.window_count == 8u);
    EXPECT(again.l1.window_count == 8u);
    EXPECT(again.l2.window_count == 5u);
    ear_obs_start_retain_reset();
}

static void test_retain_full_does_not_publish_or_clobber(void)
{
    ear_obs_start_retain_reset();
    g_builds = 0;
    g_ring_rms = -10.0f;
    ear_obs_start_measure_t a;
    ear_obs_start_measure_t b;
    EXPECT(ear_obs_start_prepare(
               "live-a", 1u, 1000, 1000, 1000, build_from_ring, NULL, &a, NULL
           )
           == EAR_OBS_START_PREPARE_READY);
    g_ring_rms = -20.0f;
    EXPECT(ear_obs_start_prepare(
               "live-b", 1u, 2000, 2000, 1000, build_from_ring, NULL, &b, NULL
           )
           == EAR_OBS_START_PREPARE_READY);
    const int builds_after_two = g_builds;
    g_ring_rms = -99.0f;
    ear_obs_start_measure_t discarded;
    memset(&discarded, 0, sizeof(discarded));
    discarded.l0.rms_mean_dbfs = 55.0f;
    EXPECT(ear_obs_start_prepare(
               "live-c", 1u, 3000, 3000, 1000, build_from_ring, NULL, &discarded, NULL
           )
           == EAR_OBS_START_PREPARE_RETAIN_FULL);
    EXPECT(g_builds == builds_after_two);
    EXPECT(discarded.l0.rms_mean_dbfs == 55.0f);
    ear_obs_start_measure_t still;
    EXPECT(ear_obs_start_retain_find("live-a", 1u, &still));
    EXPECT(still.l0.rms_mean_dbfs == -10.0f);
    EXPECT(ear_obs_start_retain_find("live-b", 1u, &still));
    EXPECT(still.l0.rms_mean_dbfs == -20.0f);

    ear_obs_start_retain_drop("live-a", 1u);
    g_ring_rms = -30.0f;
    EXPECT(ear_obs_start_prepare(
               "live-c", 1u, 3000, 3000, 1000, build_from_ring, NULL, &discarded, NULL
           )
           == EAR_OBS_START_PREPARE_READY);
    EXPECT(discarded.l0.rms_mean_dbfs == -30.0f);
    EXPECT(ear_obs_start_retain_find("live-b", 1u, &still));
    EXPECT(still.l0.rms_mean_dbfs == -20.0f);
    ear_obs_start_retain_reset();
}

static int g_depth;
static int g_takes;
static int g_gives;
static int g_reentered;
static bool g_slot_accepted;
static bool g_accepted_at_first_unlock;
static int g_give_n;
static int g_start_bodies;
static bool g_binding_live;

static void test_take(void *ctx)
{
    (void)ctx;
    if (g_depth != 0) {
        g_reentered++;
    }
    g_depth++;
    g_takes++;
}

static void test_give(void *ctx)
{
    (void)ctx;
    g_give_n++;
    if (g_give_n == 1) {
        g_accepted_at_first_unlock = g_slot_accepted;
    }
    if (g_depth > 0) {
        g_depth--;
    }
    g_gives++;
}

static void reset_lock(void)
{
    g_depth = 0;
    g_takes = 0;
    g_gives = 0;
    g_reentered = 0;
    g_slot_accepted = false;
    g_accepted_at_first_unlock = false;
    g_give_n = 0;
    g_start_bodies = 0;
    g_binding_live = true;
}

static void start_body(void *ctx)
{
    (void)ctx;
    g_start_bodies++;
    if (!g_binding_live) {
        return;
    }
    g_slot_accepted = true;
}

static void ota_body(void *ctx)
{
    sp_obs_emit_gate_t *gate = (sp_obs_emit_gate_t *)ctx;
    *gate = SP_OBS_GATE_CLOSED;
    g_binding_live = false;
}

static void reenter_body(void *ctx)
{
    ear_obs_lifecycle_lock_ops_t *ops = (ear_obs_lifecycle_lock_ops_t *)ctx;
    ops->take(ops->lock_ctx);
    ops->give(ops->lock_ctx);
}

static void test_ota_order(void)
{
    ear_obs_lifecycle_lock_ops_t ops = {
        .take = test_take,
        .give = test_give,
        .lock_ctx = NULL,
    };
    sp_obs_emit_gate_t gate = SP_OBS_GATE_RUN;

    reset_lock();
    EXPECT(ear_obs_lifecycle_section(&ops, &gate, start_body, NULL)
           == EAR_OBS_LIFECYCLE_SECTION_RAN);
    EXPECT(g_accepted_at_first_unlock);
    EXPECT(g_start_bodies == 1);
    EXPECT(g_takes == 1);
    EXPECT(g_gives == 1);
    EXPECT(g_depth == 0);
    EXPECT(g_reentered == 0);
    const int starts_after_accept = g_start_bodies;
    EXPECT(ear_obs_lifecycle_section(&ops, NULL, ota_body, &gate)
           == EAR_OBS_LIFECYCLE_SECTION_RAN);
    EXPECT(gate == SP_OBS_GATE_CLOSED);
    EXPECT(!g_binding_live);
    EXPECT(ear_obs_lifecycle_section(&ops, &gate, start_body, NULL)
           == EAR_OBS_LIFECYCLE_SECTION_SKIPPED_GATE);
    EXPECT(g_start_bodies == starts_after_accept);
    EXPECT(g_takes == g_gives);
    EXPECT(g_depth == 0);

    reset_lock();
    gate = SP_OBS_GATE_RUN;
    EXPECT(ear_obs_lifecycle_section(&ops, NULL, ota_body, &gate)
           == EAR_OBS_LIFECYCLE_SECTION_RAN);
    EXPECT(ear_obs_lifecycle_section(&ops, &gate, start_body, NULL)
           == EAR_OBS_LIFECYCLE_SECTION_SKIPPED_GATE);
    EXPECT(g_start_bodies == 0);
    EXPECT(!g_slot_accepted);
    EXPECT(!g_accepted_at_first_unlock);
    EXPECT(g_takes == g_gives);
    EXPECT(g_reentered == 0);

    /* Marking after unlock is not acceptance at the first give. */
    reset_lock();
    gate = SP_OBS_GATE_RUN;
    EXPECT(ear_obs_lifecycle_section(&ops, &gate, NULL, NULL)
           == EAR_OBS_LIFECYCLE_SECTION_RAN);
    g_slot_accepted = true;
    EXPECT(!g_accepted_at_first_unlock);

    reset_lock();
    EXPECT(ear_obs_lifecycle_section(&ops, NULL, reenter_body, &ops)
           == EAR_OBS_LIFECYCLE_SECTION_RAN);
    EXPECT(g_reentered == 1);
    EXPECT(g_takes == g_gives);
    EXPECT(g_depth == 0);
}

int main(void)
{
    test_prepare_reuses_original_aggregate();
    test_retain_full_does_not_publish_or_clobber();
    test_ota_order();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("ok — ear_obs_start_publish\n");
    return 0;
}
