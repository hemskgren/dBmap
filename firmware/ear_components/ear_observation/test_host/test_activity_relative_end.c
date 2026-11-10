/**
 * Host tests for Relative END v1 (activity detector).
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     firmware/ear_components/ear_observation/activity_detector.c \
 *     firmware/ear_components/ear_observation/test_host/test_activity_relative_end.c \
 *     -lm -o /tmp/test_activity_relative_end && /tmp/test_activity_relative_end
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

static dbmap_activity_config_t abs_cfg(void)
{
    dbmap_activity_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enter_threshold_dbfs = -45.0f;
    cfg.exit_threshold_dbfs = -55.0f;
    cfg.min_activity_ms = 1000;
    cfg.exit_hold_ms = 1000;
    cfg.boot_settle_ms = 0;
    cfg.observation_interval_ms = 5000;
    cfg.max_invalid_hold_ms = 30000;
    cfg.relative_end_enabled = false;
    return cfg;
}

static dbmap_activity_config_t rel_cfg(void)
{
    dbmap_activity_config_t cfg = abs_cfg();
    cfg.relative_end_enabled = true;
    cfg.return_margin_db = 4.0f;
    cfg.cancel_hysteresis_db = 1.5f;
    cfg.relative_exit_hold_ms = 2000;
    cfg.min_event_contrast_db = 5.0f;
    cfg.baseline_stale_ms = 15000;
    cfg.min_activity_ms = 2000; /* START after two loud windows typically */
    return cfg;
}

static dbmap_activity_emission_t feed(
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

static dbmap_activity_emission_t feed_invalid(
    dbmap_activity_detector_t *det,
    uint32_t seq,
    int64_t t_us
)
{
    dbmap_activity_sample_t s;
    memset(&s, 0, sizeof(s));
    s.has_measurement = true;
    s.rms_dbfs = -120.0f;
    s.window_ms = 1000;
    s.measured_at_us = t_us;
    s.window_seq = seq;
    s.ms_since_i2s_enable = (uint32_t)(t_us / 1000);
    s.transport_status = "ok";
    s.microphone_status = "disconnected_suspected";
    return dbmap_activity_detector_feed(det, &s);
}

/** Push three IDLE baseline samples at -51 dBFS. */
static void seed_baseline_m51(dbmap_activity_detector_t *det, uint32_t *seq, int64_t *t)
{
    for (int i = 0; i < 3; ++i) {
        *seq += 1;
        *t += 1000000;
        dbmap_activity_emission_t e = feed(det, *seq, *t, -51.0f);
        EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
        EXPECT(det->state == DBMAP_ACTIVITY_IDLE);
    }
    EXPECT(det->baseline_count >= 3);
}

/** T1: EAR-001 loud-to-ambient → relative END. */
static void test_ear001_loud_to_ambient(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed_baseline_m51(&det, &seq, &t);

    /* Pending enter then START (min_activity 2s). */
    seq++;
    t += 1000000;
    EXPECT(feed(&det, seq, t, -35.0f).kind == DBMAP_ACTIVITY_EMIT_NONE);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_ENTER);

    seq++;
    t += 1000000;
    dbmap_activity_emission_t e = feed(&det, seq, t, -35.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(det.baseline_reliable);
    EXPECT(fabsf(det.frozen_baseline_dbfs - (-51.0f)) < 0.01f);
    /* One arming sample so far (START); not armed until second episode window. */
    EXPECT(!det.relative_armed);

    /* Second episode window at ambient: arms then enters relative PENDING_EXIT. */
    seq++;
    t += 1000000;
    e = feed(&det, seq, t, -51.0f);
    EXPECT(det.relative_armed);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);

    /* Stay at -51 (above -55) for relative hold 2s. */
    seq++;
    t += 1000000;
    e = feed(&det, seq, t, -51.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);

    seq++;
    t += 1000000;
    e = feed(&det, seq, t, -51.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(e.termination == DBMAP_ACTIVITY_TERM_NATURAL);
    EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
    EXPECT(!det.baseline_frozen);
}

/** T2: arm-before-exit on the window that completes the two-window contrast. */
static void test_arm_before_exit_ordering(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed_baseline_m51(&det, &seq, &t);

    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(!det.relative_armed);

    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.relative_armed);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
}

/** T3: relative cancel at -44; clears accumulated hold. */
static void test_relative_cancel(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed_baseline_m51(&det, &seq, &t);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);

    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.exit_hold_elapsed_us == 1000000);

    /* Cancel band: B+m+h = -45.5; -44 > -45.5 → ACTIVE. */
    seq++;
    t += 1000000;
    feed(&det, seq, t, -44.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_NONE);
    EXPECT(det.exit_hold_elapsed_us == 0);
    EXPECT(!det.exit_hold_anchor_valid);
}

/** T4: stay at -51 despite above absolute -55. */
static void test_relative_stay_above_abs(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed_baseline_m51(&det, &seq, &t);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);

    seq++;
    t += 500000; /* < 2s hold */
    feed(&det, seq, t, -51.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
}

/** T5: insufficient baseline → absolute only. */
static void test_absolute_fallback_unreliable_baseline(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    cfg.min_activity_ms = 1000;
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    /* Only two baseline samples. */
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);

    seq++;
    t += 1000000;
    dbmap_activity_emission_t e = feed(&det, seq, t, -35.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);
    EXPECT(!det.baseline_reliable);
    EXPECT(!det.relative_armed);

    /* Ambient -51 cannot absolute-exit; stay ACTIVE. */
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_NONE);

    /* Absolute silence ends. */
    seq++;
    t += 1000000;
    feed(&det, seq, t, -60.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_ABSOLUTE);
    seq++;
    t += 1000000;
    e = feed(&det, seq, t, -60.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
}

/** T6: unarmed PENDING_EXIT stays absolute even if injected samples would arm in ACTIVE. */
static void test_absolute_latch_no_mid_exit_arm(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    cfg.min_event_contrast_db = 20.0f; /* START -35 vs -51 = 16 → never arms */
    cfg.min_activity_ms = 1000;
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed_baseline_m51(&det, &seq, &t);

    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(!det.relative_armed);

    seq++;
    t += 1000000;
    feed(&det, seq, t, -60.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_ABSOLUTE);
    EXPECT(!det.relative_armed);

    /* Would-be arming energy while in PENDING_EXIT must not switch latch.
     * -51 cancels absolute (>= -55). */
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_NONE);
}

/** T7: after cancel to ACTIVE, can arm later. */
static void test_rearm_after_cancel(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed_baseline_m51(&det, &seq, &t);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.relative_armed);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);

    seq++;
    t += 1000000;
    feed(&det, seq, t, -44.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(det.relative_armed); /* still armed from episode */

    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
}

/**
 * T8: invalid windows pause hold (do not credit wall-clock across the gap).
 * Accumulated progress is preserved; latch unchanged.
 */
static void test_invalid_during_relative_pending(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed_baseline_m51(&det, &seq, &t);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
    EXPECT(det.exit_hold_elapsed_us == 0);

    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.exit_hold_elapsed_us == 1000000);
    int64_t pending_at = det.pending_since_us;

    seq++;
    t += 1000000;
    feed_invalid(&det, seq, t);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
    EXPECT(det.pending_since_us == pending_at);
    EXPECT(det.exit_hold_elapsed_us == 1000000);
    EXPECT(!det.exit_hold_anchor_valid);

    /* Resume usable: re-anchor, no END yet (still 1 s accumulated of 2 s hold). */
    seq++;
    t += 1000000;
    dbmap_activity_emission_t e = feed(&det, seq, t, -51.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_hold_elapsed_us == 1000000);
    EXPECT(det.exit_hold_anchor_valid);

    seq++;
    t += 1000000;
    e = feed(&det, seq, t, -51.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(e.termination == DBMAP_ACTIVITY_TERM_NATURAL);
}

/** T9: max invalid hold still forces END. */
static void test_max_invalid_hold_unchanged(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    cfg.min_activity_ms = 1000;
    cfg.max_invalid_hold_ms = 3000;
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed_baseline_m51(&det, &seq, &t);

    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);

    seq++;
    t += 1000000;
    feed_invalid(&det, seq, t);
    seq++;
    t += 1000000;
    feed_invalid(&det, seq, t);
    seq++;
    t += 1000000;
    dbmap_activity_emission_t e = feed_invalid(&det, seq, t);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(e.termination == DBMAP_ACTIVITY_TERM_INVALID_HOLD);
    EXPECT(!det.baseline_frozen);
}

/** T10: disabled feature matches absolute path (ambient -51 does not end). */
static void test_disabled_regression_ambient(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = abs_cfg();
    cfg.min_activity_ms = 1000;
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    for (int i = 0; i < 3; ++i) {
        seq++;
        t += 1000000;
        feed(&det, seq, t, -51.0f);
    }
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(!cfg.relative_end_enabled);
}

/** T11: next episode does not inherit prior freeze/arming/latch. */
static void test_lifecycle_cleanup_next_episode(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed_baseline_m51(&det, &seq, &t);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
    EXPECT(!det.baseline_frozen);
    EXPECT(!det.relative_armed);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_NONE);
    EXPECT(det.baseline_count == 0);

    /* New episode needs a fresh baseline (≥3) before relative can arm. */
    seed_baseline_m51(&det, &seq, &t);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    EXPECT(det.baseline_reliable);
    EXPECT(det.frozen_baseline_n >= 3);
}

/** Relative: continuous 2 s hold (enter + two 1 s stays). */
static void test_rel_continuous_2s_hold(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed_baseline_m51(&det, &seq, &t);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_hold_elapsed_us == 0);

    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.exit_hold_elapsed_us == 1000000);

    seq++;
    t += 1000000;
    dbmap_activity_emission_t e = feed(&det, seq, t, -51.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
}

/** Relative: missing seq not credited; resume then END. */
static void test_rel_missing_seq_gap_and_resume(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed_baseline_m51(&det, &seq, &t);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    uint32_t pending_seq = seq;

    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.exit_hold_elapsed_us == 1000000);

    /* Skip one sequence number; wall jumps 2 s. */
    seq += 2;
    t += 2000000;
    dbmap_activity_emission_t e = feed(&det, seq, t, -51.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_hold_elapsed_us == 1000000);
    EXPECT(det.exit_hold_anchor_seq == seq);
    EXPECT(seq == pending_seq + 3);

    seq++;
    t += 1000000;
    e = feed(&det, seq, t, -51.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
}

static void test_rel_excessive_timestamp_gap(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed_baseline_m51(&det, &seq, &t);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.exit_hold_elapsed_us == 1000000);

    seq++;
    t += 2000000; /* consecutive seq, delta 2 s > 1.5 s contig_max */
    dbmap_activity_emission_t e = feed(&det, seq, t, -51.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    EXPECT(det.exit_hold_elapsed_us == 1000000);

    seq++;
    t += 1000000;
    e = feed(&det, seq, t, -51.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
}

static void test_rel_duplicate_preserves_hold(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed_baseline_m51(&det, &seq, &t);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.exit_hold_elapsed_us == 1000000);
    int64_t elapsed = det.exit_hold_elapsed_us;
    int64_t anchor_us = det.exit_hold_anchor_us;
    uint32_t anchor_seq = det.exit_hold_anchor_seq;
    dbmap_activity_exit_latch_t latch = det.exit_latch;

    dbmap_activity_sample_t dup;
    memset(&dup, 0, sizeof(dup));
    dup.has_measurement = true;
    dup.rms_dbfs = -51.0f;
    dup.window_ms = 1000;
    dup.measured_at_us = t + 500000;
    dup.window_seq = seq;
    dup.ms_since_i2s_enable = (uint32_t)((t + 500000) / 1000);
    dup.transport_status = "ok";
    dup.microphone_status = "signal_present";
    dbmap_activity_emission_t e = dbmap_activity_detector_feed(&det, &dup);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    EXPECT(det.exit_hold_elapsed_us == elapsed);
    EXPECT(det.exit_hold_anchor_us == anchor_us);
    EXPECT(det.exit_hold_anchor_seq == anchor_seq);
    EXPECT(det.exit_latch == latch);
    EXPECT(det.exit_hold_anchor_valid);
}

static void test_rel_backward_seq_and_timestamp(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed_baseline_m51(&det, &seq, &t);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.exit_hold_elapsed_us == 1000000);
    uint32_t at_seq = seq;

    /* Backward sequence: no credit, re-anchor, stay PENDING. */
    dbmap_activity_emission_t e = feed(&det, at_seq - 1, t + 1000000, -51.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_hold_elapsed_us == 1000000);
    EXPECT(det.exit_hold_anchor_valid);
    EXPECT(det.exit_hold_anchor_seq == at_seq - 1);

    /* Fresh episode for backward-timestamp case (avoid accidental 2 s END). */
    dbmap_activity_detector_init(&det, &cfg);
    seq = 0;
    t = 0;
    seed_baseline_m51(&det, &seq, &t);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -51.0f);
    EXPECT(det.exit_hold_elapsed_us == 1000000);

    e = feed(&det, seq + 1, t - 100000, -51.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_hold_elapsed_us == 1000000);
    EXPECT(det.exit_hold_anchor_us == t - 100000);
}

/** fill_diag / format_rel_diag expose SM fields without recalculating thresholds. */
static void test_rel_diag_snapshot_and_format(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    char buf[384];
    dbmap_activity_diag_t diag;

    dbmap_activity_detector_fill_diag(&det, &diag);
    EXPECT(diag.relative_end_enabled);
    EXPECT(!diag.rel_thresholds_valid);
    EXPECT(diag.state == DBMAP_ACTIVITY_IDLE);
    EXPECT(dbmap_activity_format_rel_diag(&diag, buf, sizeof(buf)) > 0);
    EXPECT(strstr(buf, "rel_end=1") != NULL);
    EXPECT(strstr(buf, "state=IDLE") != NULL);
    EXPECT(strstr(buf, "base=n/a") != NULL);
    EXPECT(strstr(buf, "armed=0") != NULL);

    seed_baseline_m51(&det, &seq, &t);
    /* PENDING_ENTER then START (one arming sample); third loud window arms. */
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(!det.relative_armed);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(det.relative_armed);

    dbmap_activity_detector_fill_diag(&det, &diag);
    EXPECT(diag.rel_thresholds_valid);
    EXPECT(diag.relative_armed);
    EXPECT(fabsf(diag.frozen_baseline_dbfs - (-51.0f)) < 0.01f);
    EXPECT(fabsf(diag.rel_exit_dbfs - (-47.0f)) < 0.01f);   /* base + 4.0 */
    EXPECT(fabsf(diag.rel_cancel_dbfs - (-45.5f)) < 0.01f); /* + 1.5 hyst */
    EXPECT(diag.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_NONE);
    EXPECT(diag.pending_exit_enters == 0);
    EXPECT(diag.pending_exit_cancels == 0);
    EXPECT(dbmap_activity_format_rel_diag(&diag, buf, sizeof(buf)) > 0);
    EXPECT(strstr(buf, "state=ACTIVE") != NULL);
    EXPECT(strstr(buf, "armed=1") != NULL);
    EXPECT(strstr(buf, "rel_exit=-47.00") != NULL);
    EXPECT(strstr(buf, "rel_cancel=-45.50") != NULL);
    EXPECT(strstr(buf, "latch=none") != NULL);

    /* Enter relative PENDING_EXIT then cancel once. */
    seq++;
    t += 1000000;
    feed(&det, seq, t, -48.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
    EXPECT(det.pending_exit_enters == 1);

    seq++;
    t += 1000000;
    feed(&det, seq, t, -48.0f); /* credit 1 s hold */
    EXPECT(det.exit_hold_elapsed_us == 1000000);
    EXPECT(det.exit_hold_elapsed_us_max == 1000000);

    seq++;
    t += 1000000;
    feed(&det, seq, t, -40.0f); /* cancel */
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(det.pending_exit_cancels == 1);
    EXPECT(det.exit_hold_elapsed_us == 0);
    EXPECT(det.exit_hold_elapsed_us_max == 1000000); /* peak retained */

    dbmap_activity_detector_fill_diag(&det, &diag);
    EXPECT(diag.pending_exit_enters == 1);
    EXPECT(diag.pending_exit_cancels == 1);
    EXPECT(diag.exit_hold_elapsed_us_max == 1000000);
    EXPECT(dbmap_activity_format_rel_diag(&diag, buf, sizeof(buf)) > 0);
    EXPECT(strstr(buf, "pend_enter=1") != NULL);
    EXPECT(strstr(buf, "pend_cancel=1") != NULL);
    EXPECT(strstr(buf, "hold_max_ms=1000") != NULL);
}

static void test_rel_diag_disabled_format(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = abs_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    dbmap_activity_diag_t diag;
    char buf[64];
    dbmap_activity_detector_fill_diag(&det, &diag);
    EXPECT(!diag.relative_end_enabled);
    EXPECT(dbmap_activity_format_rel_diag(&diag, buf, sizeof(buf)) > 0);
    EXPECT(strstr(buf, "rel_end=0") != NULL);
    EXPECT(dbmap_activity_format_rel_diag(NULL, buf, sizeof(buf)) == -1);
    EXPECT(dbmap_activity_format_rel_diag(&diag, NULL, 8) == -1);
}

/**
 * Armed Relative END + absolute-only entry must latch ABSOLUTE and END on
 * absolute hold (not cancel via relative stay). Needs a quiet baseline so
 * abs_exit (-55) is above rel_exit (base+4); with base=-51 that band is empty.
 */
static void test_armed_absolute_only_entry_latches_absolute(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    for (int i = 0; i < 3; ++i) {
        seq++;
        t += 1000000;
        EXPECT(feed(&det, seq, t, -65.0f).kind == DBMAP_ACTIVITY_EMIT_NONE);
        EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
    }
    EXPECT(det.baseline_count >= 3);

    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_ENTER);

    seq++;
    t += 1000000;
    dbmap_activity_emission_t e = feed(&det, seq, t, -35.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);
    EXPECT(det.baseline_reliable);
    EXPECT(fabsf(det.frozen_baseline_dbfs - (-65.0f)) < 0.01f);
    EXPECT(!det.relative_armed);

    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(det.relative_armed);

    /* -58: abs (-58 < -55) yes; rel (-58 <= -61) no. */
    seq++;
    t += 1000000;
    e = feed(&det, seq, t, -58.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_ABSOLUTE);
    EXPECT(det.exit_hold_elapsed_us == 0);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);

    /* Absolute hold 1000 ms: one consecutive window completes END. */
    seq++;
    t += 1000000;
    e = feed(&det, seq, t, -58.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(e.termination == DBMAP_ACTIVITY_TERM_NATURAL);
    EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
}

/** Exact boundary: relative entry <= , cancel > . */
static void test_comparator_boundaries(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = rel_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed_baseline_m51(&det, &seq, &t);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -35.0f);

    /* Entry at exactly B+m = -47. */
    seq++;
    t += 1000000;
    feed(&det, seq, t, -47.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);

    /* Stay at exactly B+m+h = -45.5. */
    seq++;
    t += 1000000;
    feed(&det, seq, t, -45.5f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);

    /* Cancel just above -45.5. */
    seq++;
    t += 1000000;
    feed(&det, seq, t, -45.499f);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
}

int main(void)
{
    test_ear001_loud_to_ambient();
    test_arm_before_exit_ordering();
    test_relative_cancel();
    test_relative_stay_above_abs();
    test_absolute_fallback_unreliable_baseline();
    test_absolute_latch_no_mid_exit_arm();
    test_rearm_after_cancel();
    test_invalid_during_relative_pending();
    test_max_invalid_hold_unchanged();
    test_disabled_regression_ambient();
    test_lifecycle_cleanup_next_episode();
    test_armed_absolute_only_entry_latches_absolute();
    test_comparator_boundaries();
    test_rel_continuous_2s_hold();
    test_rel_missing_seq_gap_and_resume();
    test_rel_excessive_timestamp_gap();
    test_rel_duplicate_preserves_hold();
    test_rel_backward_seq_and_timestamp();
    test_rel_diag_snapshot_and_format();
    test_rel_diag_disabled_format();

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
