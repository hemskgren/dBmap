/**
 * Production-path host contract for Rel END Late Sustained Contrast Arm.
 *
 * Feeds ``dbmap_activity_detector_feed`` only (not proto_relative_activity).
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     firmware/ear_components/ear_observation/activity_detector.c \
 *     firmware/ear_components/ear_observation/test_host/test_activity_late_sustained_arm.c \
 *     -lm -o /tmp/test_activity_late_sustained_arm \
 *     && /tmp/test_activity_late_sustained_arm
 *
 * Late-arm cases enable ``relative_end_late_arm_enabled``. LSA-00 verifies
 * disabled mode remains v1-identical.
 */
#include "dbmap_activity_detector.h"

#include <math.h>
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
    cfg.relative_end_late_arm_enabled = false;
    cfg.secondary_ambient_enabled = false;
    return cfg;
}

static dbmap_activity_config_t late_cfg(void)
{
    dbmap_activity_config_t cfg = rel_cfg();
    cfg.relative_end_late_arm_enabled = true;
    return cfg;
}

static dbmap_activity_config_t a3_cfg(void)
{
    dbmap_activity_config_t cfg = late_cfg();
    cfg.secondary_ambient_enabled = true;
    cfg.ambient_jump_db = 10.0f;
    cfg.ambient_stable_windows = 5;
    cfg.ambient_stable_range_db = 3.0f;
    return cfg;
}

static dbmap_activity_emission_t feed_at(
    dbmap_activity_detector_t *det,
    uint32_t seq,
    int64_t t_us,
    float rms
)
{
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
    return dbmap_activity_detector_feed(det, &s);
}

static dbmap_activity_emission_t feed(
    dbmap_activity_detector_t *det,
    uint32_t *seq,
    int64_t *t,
    float rms
)
{
    *seq += 1;
    *t += 1000000;
    return feed_at(det, *seq, *t, rms);
}

static dbmap_activity_emission_t feed_invalid(
    dbmap_activity_detector_t *det,
    uint32_t *seq,
    int64_t *t
)
{
    *seq += 1;
    *t += 1000000;
    dbmap_activity_sample_t s;
    memset(&s, 0, sizeof(s));
    s.has_measurement = true;
    s.rms_dbfs = -120.0f;
    s.window_ms = 1000;
    s.measured_at_us = *t;
    s.window_seq = *seq;
    s.ms_since_i2s_enable = (uint32_t)(*t / 1000);
    s.transport_status = "ok";
    s.microphone_status = "disconnected_suspected";
    return dbmap_activity_detector_feed(det, &s);
}

/** Advance seq/time without feeding (models a dropped window). */
static void skip_window(uint32_t *seq, int64_t *t)
{
    *seq += 1;
    *t += 1000000;
}

static void seed(dbmap_activity_detector_t *det, uint32_t *seq, int64_t *t, float rms, int n)
{
    for (int i = 0; i < n; ++i) {
        feed(det, seq, t, rms);
        EXPECT(det->state == DBMAP_ACTIVITY_IDLE);
    }
}

/**
 * PENDING_ENTER then START. With min_activity_ms=2000 and 1 s windows,
 * two loud feeds produce START on the second; that START window is arming #1.
 */
static void start_loud(dbmap_activity_detector_t *det, uint32_t *seq, int64_t *t, float loud)
{
    feed(det, seq, t, loud);
    EXPECT(det->state == DBMAP_ACTIVITY_PENDING_ENTER);
    dbmap_activity_emission_t e = feed(det, seq, t, loud);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);
    EXPECT(det->state == DBMAP_ACTIVITY_ACTIVE);
}

/** Second usable ACTIVE window after START — completes v1 first-two arming. */
static void second_arm_window(dbmap_activity_detector_t *det, uint32_t *seq, int64_t *t, float rms)
{
    feed(det, seq, t, rms);
    EXPECT(det->state == DBMAP_ACTIVITY_ACTIVE);
}

static int settle_ended(
    dbmap_activity_detector_t *det,
    uint32_t *seq,
    int64_t *t,
    float quiet,
    int n,
    dbmap_activity_exit_latch_t *out_latch
)
{
    dbmap_activity_exit_latch_t seen = DBMAP_ACTIVITY_EXIT_LATCH_NONE;
    for (int i = 0; i < n; ++i) {
        dbmap_activity_exit_latch_t before = det->exit_latch;
        dbmap_activity_emission_t e = feed(det, seq, t, quiet);
        if (det->state == DBMAP_ACTIVITY_PENDING_EXIT || before != DBMAP_ACTIVITY_EXIT_LATCH_NONE) {
            seen = before != DBMAP_ACTIVITY_EXIT_LATCH_NONE ? before : det->exit_latch;
        }
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            if (out_latch != NULL) {
                *out_latch = e.end_latch;
            }
            return 1;
        }
    }
    if (out_latch != NULL) {
        *out_latch = seen;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* LSA-01 EAR-001-shaped: warm freeze, weak init, ≥2 consecutive loud, gap END */
/* -------------------------------------------------------------------------- */
static void test_lsa01_ear001_shaped_warm_baseline(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = late_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    /* Field-like warm ambient ≈ −46.7 */
    seed(&det, &seq, &t, -46.7f, 3);
    start_loud(&det, &seq, &t, -44.0f); /* contrast vs freeze ≈ 2.7 < 5 */
    second_arm_window(&det, &seq, &t, -44.0f);
    EXPECT(det.baseline_reliable);
    EXPECT(fabsf(det.frozen_baseline_dbfs - (-46.7f)) < 0.05f);
    EXPECT(!det.relative_armed);
    EXPECT(det.measured_contrast_db + 0.01f < cfg.min_event_contrast_db);

    /* Two consecutive loud windows (each ≥ baseline+5 ⇒ ≥ −41.7). */
    feed(&det, &seq, &t, -22.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(!det.relative_armed); /* need second consecutive */
    feed(&det, &seq, &t, -22.0f);
    EXPECT(det.relative_armed); /* late arm */
    EXPECT(det.arm_src == DBMAP_ACTIVITY_ARM_SRC_LATE);
    EXPECT(det.contrast_init_valid);
    EXPECT(det.contrast_init_db + 0.01f < cfg.min_event_contrast_db);
    EXPECT(det.contrast_late_valid);
    EXPECT(det.contrast_late_db + 0.01f >= cfg.min_event_contrast_db);
    EXPECT(fabsf(det.measured_contrast_db - det.contrast_late_db) < 0.01f);
    EXPECT(fabsf(det.frozen_baseline_dbfs - (-46.7f)) < 0.05f);

    dbmap_activity_exit_latch_t latch = DBMAP_ACTIVITY_EXIT_LATCH_NONE;
    int ended = settle_ended(&det, &seq, &t, -45.0f, 6, &latch);
    EXPECT(ended);
    EXPECT(latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
    EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
}

/* -------------------------------------------------------------------------- */
/* LSA-02 T15: single unrelated spike must not late-arm / premature END       */
/* -------------------------------------------------------------------------- */
static void test_lsa02_t15_single_spike_no_premature(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg(); /* A3 on: must stay inactive while unarmed */
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed(&det, &seq, &t, -48.0f, 3);
    start_loud(&det, &seq, &t, -44.0f);
    second_arm_window(&det, &seq, &t, -44.0f);
    EXPECT(!det.relative_armed);

    for (int i = 0; i < 8; ++i) {
        feed(&det, &seq, &t, -44.0f);
        EXPECT(!det.relative_armed);
    }

    feed(&det, &seq, &t, -25.0f); /* single spike — not sustained */
    EXPECT(!det.relative_armed);  /* v1 and v2 */

    int ended = 0;
    for (int i = 0; i < 10; ++i) {
        dbmap_activity_emission_t e = feed(&det, &seq, &t, -48.0f);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ended = 1;
            EXPECT(e.end_latch != DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);
            EXPECT(e.termination != DBMAP_ACTIVITY_TERM_SECONDARY_AMBIENT);
            break;
        }
        EXPECT(!det.relative_armed);
        EXPECT(!det.secondary_ambient_valid);
    }
    EXPECT(!ended);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(det.emissions_end == 0);
}

/* -------------------------------------------------------------------------- */
/* LSA-03 one-window E8 remains open (revised E8 needs two late loud)         */
/* -------------------------------------------------------------------------- */
static void test_lsa03_one_window_late_loud_stays_open(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = late_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed(&det, &seq, &t, -48.0f, 3);
    start_loud(&det, &seq, &t, -44.0f);
    second_arm_window(&det, &seq, &t, -44.0f);
    EXPECT(!det.relative_armed);

    feed(&det, &seq, &t, -25.0f);
    EXPECT(!det.relative_armed);

    int ended = settle_ended(&det, &seq, &t, -48.0f, 8, NULL);
    EXPECT(!ended);
    EXPECT(!det.relative_armed);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
}

/* -------------------------------------------------------------------------- */
/* LSA-04 revised E8: two consecutive late loud → late-arm + Rel END (v2)     */
/* -------------------------------------------------------------------------- */
static void test_lsa04_revised_e8_two_late_loud(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = late_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed(&det, &seq, &t, -48.0f, 3);
    start_loud(&det, &seq, &t, -44.0f);
    second_arm_window(&det, &seq, &t, -44.0f);
    EXPECT(!det.relative_armed);

    feed(&det, &seq, &t, -25.0f);
    feed(&det, &seq, &t, -25.0f);
    EXPECT(det.relative_armed);

    dbmap_activity_exit_latch_t latch = DBMAP_ACTIVITY_EXIT_LATCH_NONE;
    int ended = settle_ended(&det, &seq, &t, -48.0f, 6, &latch);
    EXPECT(ended);
    EXPECT(latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
}

/* -------------------------------------------------------------------------- */
/* LSA-05 loud | invalid | loud — must not combine across unusable            */
/* -------------------------------------------------------------------------- */
static void test_lsa05_invalid_breaks_late_streak(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = late_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed(&det, &seq, &t, -48.0f, 3);
    start_loud(&det, &seq, &t, -44.0f);
    second_arm_window(&det, &seq, &t, -44.0f);
    EXPECT(!det.relative_armed);

    feed(&det, &seq, &t, -25.0f); /* streak would be 1 under v2 */
    feed_invalid(&det, &seq, &t); /* MUST reset streak */
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    feed(&det, &seq, &t, -25.0f); /* alone again — not consecutive pair */
    EXPECT(!det.relative_armed);  /* v1 and v2 */

    int ended = settle_ended(&det, &seq, &t, -48.0f, 6, NULL);
    EXPECT(!ended);
    EXPECT(!det.relative_armed);
}

/* -------------------------------------------------------------------------- */
/* LSA-06 loud | usable below-contrast | loud — must not late-arm             */
/* -------------------------------------------------------------------------- */
static void test_lsa06_below_contrast_breaks_late_streak(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = late_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed(&det, &seq, &t, -48.0f, 3);
    start_loud(&det, &seq, &t, -44.0f);
    second_arm_window(&det, &seq, &t, -44.0f);

    feed(&det, &seq, &t, -25.0f);
    /* Usable, ACTIVE, but contrast vs −48 is 4.0 < 5 → not qualifying; reset. */
    feed(&det, &seq, &t, -44.0f);
    feed(&det, &seq, &t, -25.0f);
    EXPECT(!det.relative_armed);

    int ended = settle_ended(&det, &seq, &t, -48.0f, 6, NULL);
    EXPECT(!ended);
}

/* -------------------------------------------------------------------------- */
/* LSA-07 window_seq gap between loud windows resets streak                   */
/* -------------------------------------------------------------------------- */
static void test_lsa07_seq_gap_breaks_late_streak(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = late_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed(&det, &seq, &t, -48.0f, 3);
    start_loud(&det, &seq, &t, -44.0f);
    second_arm_window(&det, &seq, &t, -44.0f);

    feed(&det, &seq, &t, -25.0f);
    skip_window(&seq, &t); /* dropped window: non-consecutive seq */
    feed(&det, &seq, &t, -25.0f);
    /*
     * Contract: forward seq gap while ACTIVE resets late-arm streak.
     * (Production today does not special-case ACTIVE seq gaps for arming;
     * v2 must implement the reset explicitly.)
     */
    EXPECT(!det.relative_armed);

    int ended = settle_ended(&det, &seq, &t, -48.0f, 6, NULL);
    EXPECT(!ended);
}

/* -------------------------------------------------------------------------- */
/* LSA-08 no late-arm evidence updates in PENDING_EXIT                        */
/* -------------------------------------------------------------------------- */
static void test_lsa08_no_arm_update_in_pending_exit(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = late_cfg();
    /* Force unarmed absolute PENDING_EXIT: raise contrast gate. */
    cfg.min_event_contrast_db = 20.0f;
    cfg.min_activity_ms = 1000;
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed(&det, &seq, &t, -51.0f, 3);
    feed(&det, &seq, &t, -35.0f); /* START (1 s min_activity) */
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(!det.relative_armed);

    feed(&det, &seq, &t, -60.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_ABSOLUTE);
    EXPECT(!det.relative_armed);

    /*
     * One more absolute-stay window completes abs hold (1000 ms).
     * v2 must not arm from PENDING_EXIT samples (streak updates ACTIVE-only).
     */
    dbmap_activity_emission_t e = feed(&det, &seq, &t, -60.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(e.end_latch == DBMAP_ACTIVITY_EXIT_LATCH_ABSOLUTE);
    EXPECT(!det.relative_armed);
}

/* -------------------------------------------------------------------------- */
/* LSA-09 EAR-002-shaped: three strong-start Rel END cycles (no regression)   */
/* -------------------------------------------------------------------------- */
static void test_lsa09_ear002_three_burst_strong_start(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    for (int burst = 0; burst < 3; ++burst) {
        seed(&det, &seq, &t, -52.7f, 3);
        start_loud(&det, &seq, &t, -22.0f);
        second_arm_window(&det, &seq, &t, -22.0f);
        EXPECT(det.relative_armed); /* init arm */
        EXPECT(det.measured_contrast_db + 0.01f >= cfg.min_event_contrast_db);

        for (int i = 0; i < 4; ++i) {
            feed(&det, &seq, &t, -22.0f);
        }
        dbmap_activity_exit_latch_t latch = DBMAP_ACTIVITY_EXIT_LATCH_NONE;
        int ended = settle_ended(&det, &seq, &t, -54.0f, 6, &latch);
        EXPECT(ended);
        EXPECT(latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
        EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
        EXPECT(!det.baseline_frozen); /* cleaned for next episode */
    }
    EXPECT(det.emissions_end == 3);
    EXPECT(det.emissions_start == 3);
}

/* -------------------------------------------------------------------------- */
/* LSA-10 E2/T14-class: osc cancel then settle Rel END                        */
/* -------------------------------------------------------------------------- */
static void test_lsa10_oscillate_cancel_then_settle(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed(&det, &seq, &t, -51.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    second_arm_window(&det, &seq, &t, -30.0f);
    EXPECT(det.relative_armed);

    /* cancel_primary = -51+4+1.5 = -45.5; -44 cancels; -45 stays */
    for (int i = 0; i < 4; ++i) {
        feed(&det, &seq, &t, -48.0f);
        EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
        EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
        feed(&det, &seq, &t, -44.0f);
        EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
        EXPECT(det.exit_hold_elapsed_us == 0);
    }

    dbmap_activity_exit_latch_t latch = DBMAP_ACTIVITY_EXIT_LATCH_NONE;
    int ended = settle_ended(&det, &seq, &t, -51.0f, 5, &latch);
    EXPECT(ended);
    EXPECT(latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
}

/* -------------------------------------------------------------------------- */
/* LSA-11 E10 sustained loud must remain open                                 */
/* -------------------------------------------------------------------------- */
static void test_lsa11_sustained_loud_no_end(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed(&det, &seq, &t, -51.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    second_arm_window(&det, &seq, &t, -30.0f);
    EXPECT(det.relative_armed);

    for (int i = 0; i < 20; ++i) {
        dbmap_activity_emission_t e = feed(&det, &seq, &t, -35.0f);
        EXPECT(e.kind != DBMAP_ACTIVITY_EMIT_END);
        EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    }
    EXPECT(det.emissions_end == 0);
}

/* -------------------------------------------------------------------------- */
/* LSA-12 E7 A3 raised ambient when armed; unarmed weak control               */
/* -------------------------------------------------------------------------- */
static void test_lsa12_e7_a3_armed_and_unarmed_control(void)
{
    /* Armed path: quiet freeze → raised −50 → secondary END */
    {
        dbmap_activity_detector_t det;
        dbmap_activity_config_t cfg = a3_cfg();
        dbmap_activity_detector_init(&det, &cfg);
        uint32_t seq = 0;
        int64_t t = 0;
        seed(&det, &seq, &t, -70.0f, 3);
        start_loud(&det, &seq, &t, -30.0f);
        second_arm_window(&det, &seq, &t, -30.0f);
        EXPECT(det.relative_armed);

        int ended = 0;
        for (int i = 0; i < 12; ++i) {
            dbmap_activity_emission_t e = feed(&det, &seq, &t, -50.0f);
            if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
                ended = 1;
                EXPECT(e.termination == DBMAP_ACTIVITY_TERM_SECONDARY_AMBIENT);
                EXPECT(e.end_latch == DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);
                break;
            }
        }
        EXPECT(ended);
    }

    /* Unarmed weak-start control: A3 must not fire */
    {
        dbmap_activity_detector_t det;
        dbmap_activity_config_t cfg = a3_cfg();
        dbmap_activity_detector_init(&det, &cfg);
        uint32_t seq = 0;
        int64_t t = 0;
        seed(&det, &seq, &t, -48.0f, 3);
        start_loud(&det, &seq, &t, -44.0f);
        second_arm_window(&det, &seq, &t, -44.0f);
        EXPECT(!det.relative_armed);

        for (int i = 0; i < 12; ++i) {
            dbmap_activity_emission_t e = feed(&det, &seq, &t, -48.0f);
            EXPECT(e.kind != DBMAP_ACTIVITY_EMIT_END);
            EXPECT(!det.secondary_ambient_valid);
        }
        EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    }
}

/* -------------------------------------------------------------------------- */
/* LSA-13 T11-class: armed absolute-only entry keeps absolute latch/hold      */
/* -------------------------------------------------------------------------- */
static void test_lsa13_absolute_latch_while_armed(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed(&det, &seq, &t, -65.0f, 3);
    start_loud(&det, &seq, &t, -35.0f);
    second_arm_window(&det, &seq, &t, -35.0f);
    EXPECT(det.relative_armed);

    /* -58: abs yes; rel (−58 <= −65+4=−61) no → absolute latch */
    feed(&det, &seq, &t, -58.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_ABSOLUTE);

    /* Absolute stay is rms < -55 (not relative cancel band). */
    dbmap_activity_emission_t e = feed(&det, &seq, &t, -58.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(e.end_latch == DBMAP_ACTIVITY_EXIT_LATCH_ABSOLUTE);
    EXPECT(e.termination == DBMAP_ACTIVITY_TERM_NATURAL);
}

/* -------------------------------------------------------------------------- */
/* LSA-14 continuity-aware Rel hold: invalid pauses; seq gap pauses           */
/* -------------------------------------------------------------------------- */
static void test_lsa14_continuity_hold_invalid_and_gap(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed(&det, &seq, &t, -51.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    second_arm_window(&det, &seq, &t, -30.0f);
    EXPECT(det.relative_armed);

    feed(&det, &seq, &t, -51.0f);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
    feed(&det, &seq, &t, -51.0f);
    EXPECT(det.exit_hold_elapsed_us == 1000000);

    feed_invalid(&det, &seq, &t);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_hold_elapsed_us == 1000000);
    EXPECT(!det.exit_hold_anchor_valid);

    feed(&det, &seq, &t, -51.0f); /* re-anchor, no END yet */
    EXPECT(det.exit_hold_elapsed_us == 1000000);
    EXPECT(det.exit_hold_anchor_valid);

    /* Forward seq gap while PENDING_EXIT: pause (no credit). */
    skip_window(&seq, &t);
    feed(&det, &seq, &t, -51.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_hold_elapsed_us == 1000000);

    dbmap_activity_emission_t e = feed(&det, &seq, &t, -51.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(e.end_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
}

/* -------------------------------------------------------------------------- */
/* LSA-15 contrast boundary: need ≥ baseline+5; 4.9 dB does not late-arm      */
/* -------------------------------------------------------------------------- */
static void test_lsa15_contrast_boundary_warm_baseline(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = late_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed(&det, &seq, &t, -46.7f, 3);
    start_loud(&det, &seq, &t, -44.0f);
    second_arm_window(&det, &seq, &t, -44.0f);
    EXPECT(!det.relative_armed);

    /* baseline+4.9 = -41.8 — below 5 dB gate */
    feed(&det, &seq, &t, -41.8f);
    feed(&det, &seq, &t, -41.8f);
    EXPECT(!det.relative_armed); /* v1 and v2 */

    /* baseline+5.0 = -41.7 — exact boundary qualifies under >= */
    feed(&det, &seq, &t, -41.7f);
    feed(&det, &seq, &t, -41.7f);
    EXPECT(det.relative_armed);
}

/* -------------------------------------------------------------------------- */
/* LSA-16 after late-arm (v2), sticky across cancel; freeze immutable         */
/* -------------------------------------------------------------------------- */
static void test_lsa16_late_arm_sticky_and_freeze_immutable(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = late_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed(&det, &seq, &t, -48.0f, 3);
    start_loud(&det, &seq, &t, -44.0f);
    second_arm_window(&det, &seq, &t, -44.0f);

    feed(&det, &seq, &t, -25.0f);
    feed(&det, &seq, &t, -25.0f);
    EXPECT(det.relative_armed);
    EXPECT(det.arm_src == DBMAP_ACTIVITY_ARM_SRC_LATE);
    const float freeze = det.frozen_baseline_dbfs;

    feed(&det, &seq, &t, -48.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    feed(&det, &seq, &t, -40.0f); /* cancel (> -48+4+1.5=−42.5) */
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(det.relative_armed); /* sticky */
    EXPECT(fabsf(det.frozen_baseline_dbfs - freeze) < 0.01f);
}

/* -------------------------------------------------------------------------- */
/* LSA-17 diag: init arm + late-arm flag visibility                           */
/* -------------------------------------------------------------------------- */
static void test_lsa17_diag_snapshot_baseline_fields(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = late_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed(&det, &seq, &t, -51.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    second_arm_window(&det, &seq, &t, -30.0f);

    dbmap_activity_diag_t diag;
    dbmap_activity_detector_fill_diag(&det, &diag);
    EXPECT(diag.relative_end_enabled);
    EXPECT(diag.relative_end_late_arm_enabled);
    EXPECT(diag.relative_armed);
    EXPECT(diag.arm_src == DBMAP_ACTIVITY_ARM_SRC_INIT);
    EXPECT(diag.contrast_init_valid);
    EXPECT(!diag.contrast_late_valid);
    EXPECT(diag.baseline_frozen);
    EXPECT(diag.baseline_reliable);
    EXPECT(fabsf(diag.frozen_baseline_dbfs - (-51.0f)) < 0.01f);
    EXPECT(diag.measured_contrast_db + 0.01f >= 5.0f);
    EXPECT(diag.rel_thresholds_valid);
    char buf[384];
    EXPECT(dbmap_activity_format_rel_diag(&diag, buf, sizeof(buf)) > 0);
    EXPECT(strstr(buf, "armed=1") != NULL);
    EXPECT(strstr(buf, "base=-51.00") != NULL);
    EXPECT(strstr(buf, "late_arm=1") != NULL);
    EXPECT(strstr(buf, "arm_src=init") != NULL);
}

/* -------------------------------------------------------------------------- */
/* LSA-00 flag off: EAR-001-shaped stays unarmed (v1 identical)               */
/* -------------------------------------------------------------------------- */
static void test_lsa00_flag_off_matches_v1(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    EXPECT(!cfg.relative_end_late_arm_enabled);
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed(&det, &seq, &t, -46.7f, 3);
    start_loud(&det, &seq, &t, -44.0f);
    second_arm_window(&det, &seq, &t, -44.0f);
    EXPECT(!det.relative_armed);

    feed(&det, &seq, &t, -22.0f);
    feed(&det, &seq, &t, -22.0f);
    EXPECT(!det.relative_armed);
    EXPECT(det.arm_src == DBMAP_ACTIVITY_ARM_SRC_NONE);

    int ended = settle_ended(&det, &seq, &t, -45.0f, 6, NULL);
    EXPECT(!ended);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);

    dbmap_activity_diag_t diag;
    dbmap_activity_detector_fill_diag(&det, &diag);
    EXPECT(!diag.relative_end_late_arm_enabled);
    char buf[384];
    EXPECT(dbmap_activity_format_rel_diag(&diag, buf, sizeof(buf)) > 0);
    EXPECT(strstr(buf, "late_arm=0") != NULL);
}

int main(void)
{
    puts("Late Sustained Contrast Arm production host contract");

    test_lsa00_flag_off_matches_v1();
    test_lsa01_ear001_shaped_warm_baseline();
    test_lsa02_t15_single_spike_no_premature();
    test_lsa03_one_window_late_loud_stays_open();
    test_lsa04_revised_e8_two_late_loud();
    test_lsa05_invalid_breaks_late_streak();
    test_lsa06_below_contrast_breaks_late_streak();
    test_lsa07_seq_gap_breaks_late_streak();
    test_lsa08_no_arm_update_in_pending_exit();
    test_lsa09_ear002_three_burst_strong_start();
    test_lsa10_oscillate_cancel_then_settle();
    test_lsa11_sustained_loud_no_end();
    test_lsa12_e7_a3_armed_and_unarmed_control();
    test_lsa13_absolute_latch_while_armed();
    test_lsa14_continuity_hold_invalid_and_gap();
    test_lsa15_contrast_boundary_warm_baseline();
    test_lsa16_late_arm_sticky_and_freeze_immutable();
    test_lsa17_diag_snapshot_baseline_fields();

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
