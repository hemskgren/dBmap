/**
 * Offline experiments: long-running / missing-END conditions at the RMS
 * activity detector (Relative END v1). Uses the production detector only.
 *
 * Expectations are derived from docs/diagnostics/ear-relative-end-v1.md and
 * the implemented latch/hold rules — not invented timeouts.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     firmware/ear_components/ear_observation/activity_detector.c \
 *     firmware/ear_components/ear_observation/test_host/test_long_running_detector_experiments.c \
 *     -lm -o /tmp/test_long_running_detector_experiments \
 *     && /tmp/test_long_running_detector_experiments
 */
#include "dbmap_activity_detector.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_scenario_fail = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
            g_scenario_fail++;                                                 \
        }                                                                      \
    } while (0)

static const char *state_name(dbmap_activity_state_t s)
{
    switch (s) {
    case DBMAP_ACTIVITY_IDLE:
        return "IDLE";
    case DBMAP_ACTIVITY_PENDING_ENTER:
        return "PENDING_ENTER";
    case DBMAP_ACTIVITY_ACTIVE:
        return "ACTIVE";
    case DBMAP_ACTIVITY_PENDING_EXIT:
        return "PENDING_EXIT";
    default:
        return "?";
    }
}

static const char *emit_name(dbmap_activity_emit_kind_t k)
{
    switch (k) {
    case DBMAP_ACTIVITY_EMIT_NONE:
        return "-";
    case DBMAP_ACTIVITY_EMIT_START:
        return "START";
    case DBMAP_ACTIVITY_EMIT_PERIODIC:
        return "PERIODIC";
    case DBMAP_ACTIVITY_EMIT_END:
        return "END";
    default:
        return "?";
    }
}

static const char *latch_name(dbmap_activity_exit_latch_t l)
{
    return dbmap_activity_exit_latch_name(l);
}

static dbmap_activity_config_t rel_cfg(void)
{
    dbmap_activity_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enter_threshold_dbfs = -45.0f;
    cfg.exit_threshold_dbfs = -55.0f;
    cfg.min_activity_ms = 2000;
    cfg.exit_hold_ms = 1000;
    cfg.boot_settle_ms = 0;
    cfg.observation_interval_ms = 5000;
    cfg.max_invalid_hold_ms = 30000;
    cfg.relative_end_enabled = true;
    cfg.return_margin_db = 4.0f;
    cfg.cancel_hysteresis_db = 1.5f;
    cfg.relative_exit_hold_ms = 2000;
    cfg.min_event_contrast_db = 5.0f;
    cfg.baseline_stale_ms = 15000;
    return cfg;
}

typedef struct {
    dbmap_activity_state_t state;
    dbmap_activity_exit_latch_t latch;
    bool armed;
    bool baseline_reliable;
    float frozen_baseline;
    float contrast;
    int64_t hold_us;
    uint32_t pend_enters;
    uint32_t pend_cancels;
} snap_t;

static snap_t snapshot(const dbmap_activity_detector_t *det)
{
    snap_t s;
    s.state = det->state;
    s.latch = det->exit_latch;
    s.armed = det->relative_armed;
    s.baseline_reliable = det->baseline_reliable;
    s.frozen_baseline = det->frozen_baseline_dbfs;
    s.contrast = det->measured_contrast_db;
    s.hold_us = det->exit_hold_elapsed_us;
    s.pend_enters = det->pending_exit_enters;
    s.pend_cancels = det->pending_exit_cancels;
    return s;
}

static bool snap_changed(const snap_t *a, const snap_t *b, dbmap_activity_emit_kind_t emit)
{
    if (emit != DBMAP_ACTIVITY_EMIT_NONE) {
        return true;
    }
    return a->state != b->state || a->latch != b->latch || a->armed != b->armed
           || a->baseline_reliable != b->baseline_reliable
           || a->hold_us != b->hold_us || a->pend_enters != b->pend_enters
           || a->pend_cancels != b->pend_cancels;
}

static void log_transition(
    const char *tag,
    uint32_t seq,
    int64_t t_us,
    float rms,
    const snap_t *before,
    const snap_t *after,
    dbmap_activity_emit_kind_t emit
)
{
    printf(
        "  [%s] t=%llds seq=%lu rms=%6.2f  %s/%s -> %s/%s  "
        "armed=%d base_ok=%d base=%.1f contrast=%.1f hold_ms=%lld "
        "pend=%lu/%lu emit=%s\n",
        tag,
        (long long)(t_us / 1000000LL),
        (unsigned long)seq,
        (double)rms,
        state_name(before->state),
        latch_name(before->latch),
        state_name(after->state),
        latch_name(after->latch),
        after->armed ? 1 : 0,
        after->baseline_reliable ? 1 : 0,
        (double)after->frozen_baseline,
        (double)after->contrast,
        (long long)(after->hold_us / 1000LL),
        (unsigned long)after->pend_enters,
        (unsigned long)after->pend_cancels,
        emit_name(emit)
    );
}

static dbmap_activity_emission_t feed_log(
    dbmap_activity_detector_t *det,
    const char *tag,
    uint32_t seq,
    int64_t t_us,
    float rms,
    int verbose
)
{
    snap_t before = snapshot(det);
    dbmap_activity_sample_t s;
    memset(&s, 0, sizeof(s));
    s.has_measurement = true;
    s.rms_dbfs = rms;
    s.window_ms = 1000;
    s.measured_at_us = t_us;
    s.window_seq = seq;
    s.ms_since_i2s_enable = (uint32_t)(t_us / 1000);
    s.transport_status = "ok";
    s.microphone_status = "signal_present";
    dbmap_activity_emission_t e = dbmap_activity_detector_feed(det, &s);
    snap_t after = snapshot(det);
    if (verbose && snap_changed(&before, &after, e.kind)) {
        log_transition(tag, seq, t_us, rms, &before, &after, e.kind);
    }
    return e;
}

static void seed_baseline(
    dbmap_activity_detector_t *det,
    uint32_t *seq,
    int64_t *t,
    float rms,
    int n
)
{
    for (int i = 0; i < n; ++i) {
        *seq += 1;
        *t += 1000000;
        feed_log(det, "seed", *seq, *t, rms, 0);
        EXPECT(det->state == DBMAP_ACTIVITY_IDLE);
    }
}

static void start_loud_episode(
    dbmap_activity_detector_t *det,
    uint32_t *seq,
    int64_t *t,
    float loud_rms,
    int verbose
)
{
    *seq += 1;
    *t += 1000000;
    feed_log(det, "enter", *seq, *t, loud_rms, verbose);
    EXPECT(det->state == DBMAP_ACTIVITY_PENDING_ENTER);

    *seq += 1;
    *t += 1000000;
    dbmap_activity_emission_t e = feed_log(det, "start", *seq, *t, loud_rms, verbose);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);
    EXPECT(det->state == DBMAP_ACTIVITY_ACTIVE);
}

static void scenario_begin(const char *name)
{
    g_scenario_fail = 0;
    printf("\n=== %s ===\n", name);
}

static void scenario_end(const char *name, int expect_pass)
{
    if (g_scenario_fail == 0) {
        printf("RESULT %s: %s\n", name, expect_pass ? "PASS" : "PASS (observed as designed)");
    } else {
        printf("RESULT %s: FAIL (%d assertion(s))\n", name, g_scenario_fail);
    }
}

/**
 * E1: Strong event then gradual decay to ambient above absolute exit.
 * Design: relative END should fire once level stays <= baseline+margin for
 * relative_exit_hold_ms of continuous windows.
 */
static void exp_strong_then_gradual_decay(void)
{
    scenario_begin("E1 strong→gradual decay → relative END");
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    int ended = 0;

    seed_baseline(&det, &seq, &t, -51.0f, 3);
    start_loud_episode(&det, &seq, &t, -30.0f, 1);

    /* Second arming window (still loud). */
    seq++;
    t += 1000000;
    feed_log(&det, "arm2", seq, t, -30.0f, 1);
    EXPECT(det.relative_armed);
    EXPECT(det.measured_contrast_db >= 5.0f);

    /* Gradual decay -30 → -51 over 10 s (1 dB-ish steps, then settle). */
    const float steps[] = {
        -32.0f, -35.0f, -38.0f, -41.0f, -44.0f, -46.0f, -48.0f, -49.0f, -50.0f, -51.0f,
        -51.0f, -51.0f, -51.0f
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_emission_t e = feed_log(&det, "decay", seq, t, steps[i], 1);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ended = 1;
            EXPECT(e.termination == DBMAP_ACTIVITY_TERM_NATURAL);
            break;
        }
    }
    EXPECT(ended);
    EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
    scenario_end("E1", 1);
}

/**
 * E2: Oscillate around relative cancel / entry band after a loud event.
 * Cancel clears hold; short visits below entry never accumulate 2 s.
 */
static void exp_oscillate_around_relative_band(void)
{
    scenario_begin("E2 oscillate around relative entry/cancel");
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed_baseline(&det, &seq, &t, -51.0f, 3);
    start_loud_episode(&det, &seq, &t, -30.0f, 1);
    seq++;
    t += 1000000;
    feed_log(&det, "arm2", seq, t, -30.0f, 1);
    EXPECT(det.relative_armed);

    /*
     * Rel entry <= -47; cancel > -45.5.
     * Alternate 1 s in PENDING_EXIT at -48 then cancel at -44.
     * Hold never reaches 2000 ms.
     */
    int ended = 0;
    for (int i = 0; i < 8; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_emission_t e = feed_log(&det, "quiet", seq, t, -48.0f, 1);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ended = 1;
        }
        EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT
               || det.state == DBMAP_ACTIVITY_IDLE);

        seq++;
        t += 1000000;
        e = feed_log(&det, "bump", seq, t, -44.0f, 1);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ended = 1;
        }
        if (!ended) {
            EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
            EXPECT(det.exit_hold_elapsed_us == 0);
        }
    }
    EXPECT(!ended);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(det.pending_exit_cancels >= 8);
    EXPECT(det.emissions_end == 0);

    /* After oscillation stops at ambient, END should still be reachable. */
    for (int i = 0; i < 4; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_emission_t e = feed_log(&det, "settle", seq, t, -51.0f, 1);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ended = 1;
            break;
        }
    }
    EXPECT(ended);
    scenario_end("E2", 1);
}

/**
 * E3: Weak continuous activity above absolute exit that never meets contrast.
 * Design accepted limitation: Relative END stays unarmed; absolute cannot exit.
 */
static void exp_weak_continuous_no_contrast(void)
{
    scenario_begin("E3 weak continuous (no contrast) → no END");
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    /* Baseline near enter so contrast from -44 is < 5 dB. */
    seed_baseline(&det, &seq, &t, -48.0f, 3);
    start_loud_episode(&det, &seq, &t, -44.0f, 1);
    seq++;
    t += 1000000;
    feed_log(&det, "arm2", seq, t, -44.0f, 1);
    EXPECT(det.baseline_reliable);
    EXPECT(!det.relative_armed);
    EXPECT(det.measured_contrast_db < 5.0f);

    int ended = 0;
    for (int i = 0; i < 20; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_emission_t e = feed_log(&det, "weak", seq, t, -48.0f, i < 3 || i > 17);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ended = 1;
            break;
        }
    }
    EXPECT(!ended);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_NONE);
    EXPECT(det.emissions_end == 0);
    printf(
        "  note: documented v1 limitation — weak event / ambient >= abs exit sticks ACTIVE\n"
    );
    scenario_end("E3", 1);
}

/**
 * E4: Intermittent silence then renewed activity before relative hold completes.
 */
static void exp_intermittent_silence_renewed(void)
{
    scenario_begin("E4 intermittent silence + renewed activity");
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed_baseline(&det, &seq, &t, -51.0f, 3);
    start_loud_episode(&det, &seq, &t, -30.0f, 1);
    seq++;
    t += 1000000;
    feed_log(&det, "arm2", seq, t, -30.0f, 1);

    /* 1 s quiet (enter pending, hold=0) then renew before 2 s hold. */
    for (int cycle = 0; cycle < 3; ++cycle) {
        seq++;
        t += 1000000;
        feed_log(&det, "sil1", seq, t, -51.0f, 1);
        EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
        EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);

        seq++;
        t += 1000000;
        feed_log(&det, "sil2", seq, t, -51.0f, 1);
        EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
        EXPECT(det.exit_hold_elapsed_us == 1000000);

        seq++;
        t += 1000000;
        feed_log(&det, "renew", seq, t, -35.0f, 1);
        EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
        EXPECT(det.exit_hold_elapsed_us == 0);
    }

    /* Final uninterrupted quiet → END. */
    int ended = 0;
    for (int i = 0; i < 4; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_emission_t e = feed_log(&det, "final", seq, t, -51.0f, 1);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ended = 1;
            break;
        }
    }
    EXPECT(ended);
    EXPECT(det.pending_exit_cancels >= 3);
    scenario_end("E4", 1);
}

/**
 * E5: IDLE baseline candidates older than baseline_stale_ms at START.
 * Unreliable freeze → absolute-only; ambient above abs exit → no END.
 */
static void exp_stale_baseline_before_start(void)
{
    scenario_begin("E5 stale pre-event baseline → absolute-only stick");
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    /* Three ambient samples, then a gap > 15 s with no IDLE below-enter push
     * that would refresh them — advance time then START immediately. */
    seed_baseline(&det, &seq, &t, -51.0f, 3);
    EXPECT(det.baseline_count >= 3);

    /* Jump 20 s (no samples). Stale drop happens at freeze on START. */
    t += 20000000;

    seq++;
    t += 1000000;
    feed_log(&det, "enter", seq, t, -30.0f, 1);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_ENTER);

    seq++;
    t += 1000000;
    dbmap_activity_emission_t e = feed_log(&det, "start", seq, t, -30.0f, 1);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);
    EXPECT(det.baseline_frozen);
    EXPECT(!det.baseline_reliable);
    EXPECT(det.frozen_baseline_n == 0);

    seq++;
    t += 1000000;
    feed_log(&det, "arm2", seq, t, -30.0f, 1);
    EXPECT(!det.relative_armed);

    int ended = 0;
    for (int i = 0; i < 10; ++i) {
        seq++;
        t += 1000000;
        e = feed_log(&det, "amb", seq, t, -51.0f, 1);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ended = 1;
            break;
        }
    }
    EXPECT(!ended);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    printf(
        "  note: stale IDLE candidates dropped → unreliable baseline; "
        "absolute exit never met at -51\n"
    );
    scenario_end("E5", 1);
}

/**
 * E6: Absolute END path — deep silence below abs exit, relative unarmed.
 */
static void exp_absolute_end_deep_silence(void)
{
    scenario_begin("E6 absolute END via deep silence (unarmed)");
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    /* Only two baseline samples → unreliable. */
    seed_baseline(&det, &seq, &t, -51.0f, 2);
    start_loud_episode(&det, &seq, &t, -30.0f, 1);
    EXPECT(!det.baseline_reliable);

    seq++;
    t += 1000000;
    feed_log(&det, "loud2", seq, t, -30.0f, 1);

    seq++;
    t += 1000000;
    feed_log(&det, "abs", seq, t, -60.0f, 1);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_ABSOLUTE);

    seq++;
    t += 1000000;
    dbmap_activity_emission_t e = feed_log(&det, "abs2", seq, t, -60.0f, 1);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(e.termination == DBMAP_ACTIVITY_TERM_NATURAL);
    scenario_end("E6", 1);
}

/**
 * E7: Quiet frozen baseline, then ambient rises above both exit paths.
 * Design accepted limitation: no secondary ambient exit.
 */
static void exp_ambient_rise_after_quiet_baseline(void)
{
    scenario_begin("E7 ambient rise after quiet baseline → stick");
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed_baseline(&det, &seq, &t, -70.0f, 3);
    start_loud_episode(&det, &seq, &t, -30.0f, 1);
    seq++;
    t += 1000000;
    feed_log(&det, "arm2", seq, t, -30.0f, 1);
    EXPECT(det.relative_armed);
    /* rel_exit = -66; abs_exit = -55 */

    /* Settle at raised ambient -50: not relative (<= -66) nor absolute (< -55). */
    int ended = 0;
    for (int i = 0; i < 15; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_emission_t e = feed_log(&det, "raised", seq, t, -50.0f, i < 2 || i > 12);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ended = 1;
            break;
        }
    }
    EXPECT(!ended);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(det.relative_armed);
    printf(
        "  note: documented v1 limitation — no secondary ambient exit if "
        "background rises during episode (base=-70, ambient=-50)\n"
    );
    scenario_end("E7", 1);
}

/**
 * E8: Late loud burst after weak first-two windows does not re-arm.
 */
static void exp_late_loud_does_not_rearm(void)
{
    scenario_begin("E8 late loud after weak first-two → stays unarmed");
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed_baseline(&det, &seq, &t, -48.0f, 3);
    start_loud_episode(&det, &seq, &t, -44.0f, 1);
    seq++;
    t += 1000000;
    feed_log(&det, "arm2", seq, t, -44.0f, 1);
    EXPECT(!det.relative_armed);

    /* Late loud burst — contrast from first two is fixed; still unarmed. */
    seq++;
    t += 1000000;
    feed_log(&det, "late", seq, t, -25.0f, 1);
    EXPECT(!det.relative_armed);
    EXPECT(det.measured_contrast_db < 5.0f);

    int ended = 0;
    for (int i = 0; i < 8; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_emission_t e = feed_log(&det, "amb", seq, t, -48.0f, 1);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ended = 1;
            break;
        }
    }
    EXPECT(!ended);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    printf(
        "  note: arming uses only first two episode windows; late contrast ignored\n"
    );
    scenario_end("E8", 1);
}

/**
 * E9: Absolute-latched exit cancelled by return above -55; then stuck above
 * relative exit because quiet baseline makes relative entry unreachable.
 */
static void exp_abs_latch_cancel_then_raised_ambient(void)
{
    scenario_begin("E9 abs latch cancel → raised ambient stick");
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed_baseline(&det, &seq, &t, -70.0f, 3);
    start_loud_episode(&det, &seq, &t, -30.0f, 1);
    seq++;
    t += 1000000;
    feed_log(&det, "arm2", seq, t, -30.0f, 1);
    EXPECT(det.relative_armed);

    /* -58: abs yes, rel (-58 <= -66) no → absolute latch. */
    seq++;
    t += 1000000;
    feed_log(&det, "abse", seq, t, -58.0f, 1);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_ABSOLUTE);

    /* Cancel before 1 s absolute hold by returning to -50. */
    seq++;
    t += 1000000;
    feed_log(&det, "cancel", seq, t, -50.0f, 1);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_NONE);
    EXPECT(det.pending_exit_cancels >= 1);

    int ended = 0;
    for (int i = 0; i < 10; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_emission_t e = feed_log(&det, "stuck", seq, t, -50.0f, 1);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ended = 1;
            break;
        }
    }
    EXPECT(!ended);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    scenario_end("E9", 1);
}

/**
 * E10: Plateau in the relative deadband (-46.9 .. -45.5] after arming:
 * cannot enter PENDING_EXIT (need <= -47) and not absolute.
 */
static void exp_plateau_in_relative_deadband(void)
{
    scenario_begin("E10 plateau in relative deadband → no pending exit");
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed_baseline(&det, &seq, &t, -51.0f, 3);
    start_loud_episode(&det, &seq, &t, -30.0f, 1);
    seq++;
    t += 1000000;
    feed_log(&det, "arm2", seq, t, -30.0f, 1);
    EXPECT(det.relative_armed);
    /* entry -47, cancel -45.5; -46.0 is above entry → stay ACTIVE */

    int ended = 0;
    for (int i = 0; i < 12; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_emission_t e = feed_log(&det, "band", seq, t, -46.0f, 1);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ended = 1;
            break;
        }
        EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
        EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_NONE);
    }
    EXPECT(!ended);
    EXPECT(det.pending_exit_enters == 0);
    printf(
        "  note: -46 is louder than rel entry (-47) and quieter than enter (-45); "
        "ACTIVE continues with PERIODICs until deeper return or abs exit\n"
    );
    scenario_end("E10", 1);
}

int main(void)
{
    puts("long-running detector experiments (Relative END v1 defaults)");
    puts("thresholds: enter=-45 exit=-55 rel_margin=4 cancel_hyst=1.5 "
         "rel_hold=2000 abs_hold=1000 contrast=5 stale=15000");

    exp_strong_then_gradual_decay();
    exp_oscillate_around_relative_band();
    exp_weak_continuous_no_contrast();
    exp_intermittent_silence_renewed();
    exp_stale_baseline_before_start();
    exp_absolute_end_deep_silence();
    exp_ambient_rise_after_quiet_baseline();
    exp_late_loud_does_not_rearm();
    exp_abs_latch_cancel_then_raised_ambient();
    exp_plateau_in_relative_deadband();

    printf("\n=== summary ===\n");
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("all experiment assertions held");
    return 0;
}
