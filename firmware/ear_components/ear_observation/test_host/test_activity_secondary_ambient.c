/**
 * Host tests for Rel END v2 A3 — Secondary Ambient Exit (production detector).
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     firmware/ear_components/ear_observation/activity_detector.c \
 *     firmware/ear_components/ear_observation/test_host/test_activity_secondary_ambient.c \
 *     -lm -o /tmp/test_activity_secondary_ambient \
 *     && /tmp/test_activity_secondary_ambient
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

static dbmap_activity_config_t v1_cfg(void)
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

static dbmap_activity_config_t a3_cfg(void)
{
    dbmap_activity_config_t cfg = v1_cfg();
    cfg.secondary_ambient_enabled = true;
    cfg.ambient_jump_db = 10.0f;
    cfg.ambient_stable_windows = 5;
    cfg.ambient_stable_range_db = 3.0f;
    return cfg;
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
    dbmap_activity_sample_t s;
    memset(&s, 0, sizeof(s));
    s.has_measurement = true;
    s.rms_dbfs = rms;
    s.window_ms = 1000;
    s.measured_at_us = *t;
    s.window_seq = *seq;
    s.ms_since_i2s_enable = (uint32_t)(*t / 1000);
    s.transport_status = "ok";
    s.microphone_status = "signal_present";
    return dbmap_activity_detector_feed(det, &s);
}

static void seed(dbmap_activity_detector_t *det, uint32_t *seq, int64_t *t, float rms, int n)
{
    for (int i = 0; i < n; ++i) {
        feed(det, seq, t, rms);
        EXPECT(det->state == DBMAP_ACTIVITY_IDLE);
    }
}

static void start_loud(dbmap_activity_detector_t *det, uint32_t *seq, int64_t *t, float loud)
{
    feed(det, seq, t, loud);
    EXPECT(det->state == DBMAP_ACTIVITY_PENDING_ENTER);
    dbmap_activity_emission_t e = feed(det, seq, t, loud);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);
    EXPECT(det->state == DBMAP_ACTIVITY_ACTIVE);
}

static void arm2(dbmap_activity_detector_t *det, uint32_t *seq, int64_t *t, float loud)
{
    feed(det, seq, t, loud);
    EXPECT(det->relative_armed);
}

/** E7: quiet baseline −70, settle raised ambient −50 → secondary END. */
static void test_e7_quiet_baseline_raised_ambient(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    seed(&det, &seq, &t, -70.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    arm2(&det, &seq, &t, -30.0f);
    EXPECT(fabsf(det.frozen_baseline_dbfs - (-70.0f)) < 0.01f);
    EXPECT(!det.secondary_ambient_valid);

    /* First 4 stable −50 windows: collecting; not yet valid. */
    for (int i = 0; i < 4; ++i) {
        feed(&det, &seq, &t, -50.0f);
        EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
        EXPECT(!det.secondary_ambient_valid);
    }
    /*
     * 5th stable window: secondary becomes valid and, on the same call,
     * secondary entry (−50 <= −50+4) latches PENDING_EXIT/SECONDARY.
     */
    feed(&det, &seq, &t, -50.0f);
    EXPECT(det.secondary_ambient_valid);
    EXPECT(fabsf(det.secondary_ambient_dbfs - (-50.0f)) < 0.01f);
    EXPECT(fabsf(det.frozen_baseline_dbfs - (-70.0f)) < 0.01f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);
    EXPECT(det.exit_hold_elapsed_us == 0);

    feed(&det, &seq, &t, -50.0f); /* hold 1s */
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    dbmap_activity_emission_t e = feed(&det, &seq, &t, -50.0f); /* hold 2s → END */
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(e.termination == DBMAP_ACTIVITY_TERM_SECONDARY_AMBIENT);
    EXPECT(e.end_latch == DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);
    EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
    EXPECT(!det.secondary_ambient_valid);
}

/** Same sequence with A3 off must remain ACTIVE (V1 reference). */
static void test_e7_v1_reference_stays_open(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = v1_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed(&det, &seq, &t, -70.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    arm2(&det, &seq, &t, -30.0f);
    for (int i = 0; i < 20; ++i) {
        dbmap_activity_emission_t e = feed(&det, &seq, &t, -50.0f);
        EXPECT(e.kind != DBMAP_ACTIVITY_EMIT_END);
        EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
        EXPECT(!det.secondary_ambient_valid);
    }
}

/** Sudden ambient step to −50 (T12). */
static void test_t12_sudden_ambient_step(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed(&det, &seq, &t, -70.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    arm2(&det, &seq, &t, -30.0f);
    for (int i = 0; i < 3; ++i) {
        feed(&det, &seq, &t, -30.0f);
    }
    int ended = 0;
    for (int i = 0; i < 12; ++i) {
        dbmap_activity_emission_t e = feed(&det, &seq, &t, -50.0f);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            EXPECT(e.termination == DBMAP_ACTIVITY_TERM_SECONDARY_AMBIENT);
            EXPECT(e.end_latch == DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);
            ended = 1;
            break;
        }
    }
    EXPECT(ended);
}

/** Long quiet-baseline episode at −52 (T13). */
static void test_t13_long_quiet_baseline_episode(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed(&det, &seq, &t, -70.0f, 3);
    start_loud(&det, &seq, &t, -28.0f);
    arm2(&det, &seq, &t, -28.0f);
    int ended = 0;
    for (int i = 0; i < 20; ++i) {
        dbmap_activity_emission_t e = feed(&det, &seq, &t, -52.0f);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            EXPECT(e.end_latch == DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);
            ended = 1;
            break;
        }
    }
    EXPECT(ended);
}

/** Gradual decay to stable −50 plateau (not primary relative for base −70). */
static void test_gradual_decay_to_m50_plateau(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed(&det, &seq, &t, -70.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    arm2(&det, &seq, &t, -30.0f);
    const float decay[] = {-35, -40, -44, -46, -48, -49, -50};
    for (size_t i = 0; i < sizeof(decay) / sizeof(decay[0]); ++i) {
        feed(&det, &seq, &t, decay[i]);
    }
    int ended = 0;
    for (int i = 0; i < 10; ++i) {
        dbmap_activity_emission_t e = feed(&det, &seq, &t, -50.0f);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            EXPECT(e.end_latch == DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);
            ended = 1;
            break;
        }
    }
    EXPECT(ended);
}

/** Genuine sustained −50 after strong start: A3 will END — document risk. */
static void test_sustained_m50_after_strong_ends_via_a3(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed(&det, &seq, &t, -70.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    arm2(&det, &seq, &t, -30.0f);
    int ended = 0;
    for (int i = 0; i < 15; ++i) {
        dbmap_activity_emission_t e = feed(&det, &seq, &t, -50.0f);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            EXPECT(e.end_latch == DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);
            ended = 1;
            break;
        }
    }
    EXPECT(ended);
    /* Risk: RMS-stable −50 after a loud start is treated as raised ambient. */
}

/** New loud sound during observation cancels secondary pending. */
static void test_a3_cancel_then_renewed_activity(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed(&det, &seq, &t, -70.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    arm2(&det, &seq, &t, -30.0f);
    for (int i = 0; i < 5; ++i) {
        feed(&det, &seq, &t, -50.0f);
    }
    EXPECT(det.secondary_ambient_valid);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);
    /* Renewed loud: cancel secondary ( > sec_cancel = −50+4+1.5 = −44.5 ). */
    feed(&det, &seq, &t, -35.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_NONE);
    EXPECT(det.pending_exit_cancels >= 1);
    /* Still armed; secondary may still be valid; can re-enter later. */
    EXPECT(det.relative_armed);
}

/**
 * E2/T14-style oscillation around the *secondary* cancel line.
 *
 * With sec_ambient = −50, return_margin = 4, cancel_hysteresis = 1.5:
 *   stay  ⇔ rms <= −50+4+1.5 = −44.5
 *   cancel ⇔ rms >  −44.5
 * Stay sample −46; cancel sample −44 (just above the boundary).
 */
static void test_secondary_latch_oscillation_cancel_then_end(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    const float base = -70.0f;
    const float loud = -30.0f;
    const float plateau = -50.0f; /* qualifies A3: jump 20, range 0 */
    const float stay = -46.0f;    /* <= sec_cancel −44.5 */
    const float cancel = -44.0f;  /* >  sec_cancel −44.5 */

    seed(&det, &seq, &t, base, 3);
    start_loud(&det, &seq, &t, loud);
    arm2(&det, &seq, &t, loud);
    EXPECT(det.relative_armed);
    EXPECT(fabsf(det.frozen_baseline_dbfs - base) < 0.01f);

    for (int i = 0; i < 5; ++i) {
        feed(&det, &seq, &t, plateau);
    }
    EXPECT(det.secondary_ambient_valid);
    EXPECT(fabsf(det.secondary_ambient_dbfs - plateau) < 0.01f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);

    const uint32_t cancels_before = det.pending_exit_cancels;
    for (int i = 0; i < 4; ++i) {
        feed(&det, &seq, &t, stay);
        EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
        EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);

        feed(&det, &seq, &t, cancel);
        EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
        EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_NONE);
        EXPECT(det.exit_hold_elapsed_us == 0);
        EXPECT(det.pending_exit_cancels > cancels_before);
    }
    EXPECT(det.pending_exit_cancels >= cancels_before + 4);
    EXPECT(det.emissions_end == 0);
    EXPECT(det.relative_armed);
    /* Secondary reference is not cleared by cancel; freeze unchanged. */
    EXPECT(det.secondary_ambient_valid);
    EXPECT(fabsf(det.frozen_baseline_dbfs - base) < 0.01f);

    /* Re-enter secondary pending and complete relative-hold END. */
    feed(&det, &seq, &t, plateau);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);
    EXPECT(det.exit_hold_elapsed_us == 0);
    feed(&det, &seq, &t, plateau);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    dbmap_activity_emission_t e = feed(&det, &seq, &t, plateau);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(e.termination == DBMAP_ACTIVITY_TERM_SECONDARY_AMBIENT);
    EXPECT(e.end_latch == DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);
    EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
}

/**
 * T15 — weak initial activity then unrelated strong spike.
 *
 * Design-study sequence: freeze −48, start −44 (contrast 4 < min_event_contrast 5),
 * continue weak, spike −25, settle −48. Arming never succeeds; A3 must not emit a
 * premature secondary END. Documents current unarmed-stick behavior (no production
 * arming change).
 */
static void test_t15_weak_then_spike_no_premature_a3_end(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    const float base = -48.0f;
    const float weak = -44.0f; /* contrast vs base = 4.0 < 5.0 */
    const float spike = -25.0f;
    const float settle = -48.0f;

    seed(&det, &seq, &t, base, 3);
    start_loud(&det, &seq, &t, weak);
    feed(&det, &seq, &t, weak); /* second arming window */
    EXPECT(det.baseline_reliable);
    EXPECT(!det.relative_armed);
    EXPECT(det.measured_contrast_db < cfg.min_event_contrast_db);
    EXPECT(det.measured_contrast_db + 0.01f >= 4.0f);

    for (int i = 0; i < 8; ++i) {
        dbmap_activity_emission_t e = feed(&det, &seq, &t, weak);
        EXPECT(e.kind != DBMAP_ACTIVITY_EMIT_END);
        EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
        EXPECT(!det.relative_armed);
    }

    feed(&det, &seq, &t, spike);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(!det.relative_armed); /* late loud does not re-arm */

    int ended = 0;
    for (int i = 0; i < 10; ++i) {
        dbmap_activity_emission_t e = feed(&det, &seq, &t, settle);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ended = 1;
            /* Document path if production ever ends here — must not be A3. */
            EXPECT(e.end_latch != DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);
            EXPECT(e.termination != DBMAP_ACTIVITY_TERM_SECONDARY_AMBIENT);
            break;
        }
        EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
        EXPECT(!det.relative_armed);
        /* Jump vs freeze is 0 at settle=base; secondary must not qualify. */
        EXPECT(!det.secondary_ambient_valid);
    }
    EXPECT(!ended);
    EXPECT(det.emissions_end == 0);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(!det.relative_armed);
    EXPECT(!det.secondary_ambient_valid);
    /*
     * Observed current behavior: episode remains ACTIVE (unarmed absolute-only).
     * Settle −48 is above abs exit (−55), so no absolute END either — accepted
     * weak-contrast stick; A3 correctly stays inactive while unarmed.
     */
}

/**
 * Primary relative wins when *both* relative and secondary entry qualify.
 *
 * Construction (from cfg thresholds):
 *   freeze B=−56 → rel entry rms <= −52; abs exit rms < −55
 *   plateau −46 → jump (−46)−(−56)=10 qualifies secondary; first latch SECONDARY
 *   cancel loud, then feed −52:
 *     below_rel (−52 <= −52), below_sec (−52 <= −42), not abs (−52 !< −55)
 *   choose_exit_latch must select RELATIVE.
 */
static void test_primary_relative_wins_when_both_entry_qualify(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;

    const float base = -56.0f;
    const float loud = -30.0f;
    const float sec_plateau = -46.0f; /* jump 10; below enter */
    const float both_entry = -52.0f;  /* rel+sec, not absolute */

    seed(&det, &seq, &t, base, 3);
    start_loud(&det, &seq, &t, loud);
    arm2(&det, &seq, &t, loud);
    EXPECT(det.relative_armed);
    EXPECT(fabsf(det.frozen_baseline_dbfs - base) < 0.01f);

    for (int i = 0; i < 5; ++i) {
        feed(&det, &seq, &t, sec_plateau);
    }
    EXPECT(det.secondary_ambient_valid);
    EXPECT(fabsf(det.secondary_ambient_dbfs - sec_plateau) < 0.01f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);

    feed(&det, &seq, &t, loud); /* cancel secondary pending */
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_NONE);
    EXPECT(det.secondary_ambient_valid); /* still valid for dual-entry */

    feed(&det, &seq, &t, both_entry);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
    EXPECT(det.exit_latch != DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);
    EXPECT(det.secondary_ambient_valid);
    EXPECT(fabsf(det.frozen_baseline_dbfs - base) < 0.01f);

    feed(&det, &seq, &t, both_entry);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    dbmap_activity_emission_t e = feed(&det, &seq, &t, both_entry);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(e.termination == DBMAP_ACTIVITY_TERM_NATURAL);
    EXPECT(e.end_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
    EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
}

/** Stability range outside 3 dB must not arm secondary. */
static void test_stability_range_outside(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed(&det, &seq, &t, -70.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    arm2(&det, &seq, &t, -30.0f);

    /* Span 4 dB > 3. */
    const float wide[] = {-52.0f, -50.0f, -49.0f, -48.0f, -48.0f};
    for (size_t i = 0; i < 5; ++i) {
        feed(&det, &seq, &t, wide[i]);
    }
    EXPECT(!det.secondary_ambient_valid);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
}

/** Stability range inside 3 dB arms secondary (and may enter PENDING). */
static void test_stability_range_inside(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed(&det, &seq, &t, -70.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    arm2(&det, &seq, &t, -30.0f);

    const float tight[] = {-51.0f, -50.5f, -50.0f, -49.5f, -49.0f};
    for (size_t i = 0; i < 5; ++i) {
        feed(&det, &seq, &t, tight[i]);
    }
    EXPECT(det.secondary_ambient_valid);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY);
}

/** Jump threshold: just below / at / above 10 dB. */
static void test_jump_threshold_boundaries(void)
{
    /* Below: base −55, plateau −46 → jump 9 < 10. */
    {
        dbmap_activity_detector_t det;
        dbmap_activity_config_t cfg = a3_cfg();
        dbmap_activity_detector_init(&det, &cfg);
        uint32_t seq = 0;
        int64_t t = 0;
        seed(&det, &seq, &t, -55.0f, 3);
        start_loud(&det, &seq, &t, -30.0f);
        arm2(&det, &seq, &t, -30.0f);
        for (int i = 0; i < 8; ++i) {
            feed(&det, &seq, &t, -46.0f);
        }
        EXPECT(!det.secondary_ambient_valid);
    }
    /* At: base −60, plateau −50 → jump 10. */
    {
        dbmap_activity_detector_t det;
        dbmap_activity_config_t cfg = a3_cfg();
        dbmap_activity_detector_init(&det, &cfg);
        uint32_t seq = 0;
        int64_t t = 0;
        seed(&det, &seq, &t, -60.0f, 3);
        start_loud(&det, &seq, &t, -30.0f);
        arm2(&det, &seq, &t, -30.0f);
        for (int i = 0; i < 5; ++i) {
            feed(&det, &seq, &t, -50.0f);
        }
        EXPECT(det.secondary_ambient_valid);
        EXPECT(fabsf(det.secondary_ambient_dbfs - (-50.0f)) < 0.01f);
        EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY
               || det.state == DBMAP_ACTIVITY_PENDING_EXIT
               || det.state == DBMAP_ACTIVITY_ACTIVE);
    }
    /* Above already covered by E7 (jump 20). */
}

/** Unreliable/stale baseline: A3 must not activate. */
static void test_stale_baseline_a3_inactive(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed(&det, &seq, &t, -70.0f, 3);
    t += 20000000; /* > stale 15s */
    start_loud(&det, &seq, &t, -30.0f);
    EXPECT(!det.baseline_reliable);
    feed(&det, &seq, &t, -30.0f);
    EXPECT(!det.relative_armed);
    for (int i = 0; i < 15; ++i) {
        feed(&det, &seq, &t, -50.0f);
        EXPECT(!det.secondary_ambient_valid);
        EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    }
}

/** Primary relative still preferred when both entry conditions true. */
static void test_primary_relative_unchanged_ear001(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed(&det, &seq, &t, -51.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    arm2(&det, &seq, &t, -30.0f);
    feed(&det, &seq, &t, -51.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
    feed(&det, &seq, &t, -51.0f);
    dbmap_activity_emission_t e = feed(&det, &seq, &t, -51.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(e.termination == DBMAP_ACTIVITY_TERM_NATURAL);
    EXPECT(e.end_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
}

/** Absolute deep silence still absolute. */
static void test_absolute_deep_silence_unchanged(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed(&det, &seq, &t, -51.0f, 2);
    start_loud(&det, &seq, &t, -30.0f);
    EXPECT(!det.baseline_reliable);
    feed(&det, &seq, &t, -30.0f);
    feed(&det, &seq, &t, -60.0f);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_ABSOLUTE);
    dbmap_activity_emission_t e = feed(&det, &seq, &t, -60.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(e.end_latch == DBMAP_ACTIVITY_EXIT_LATCH_ABSOLUTE);
    EXPECT(e.termination == DBMAP_ACTIVITY_TERM_NATURAL);
}

/** Oscillation around relative cancel still cancels (T14-like). */
static void test_relative_oscillation_preserved(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed(&det, &seq, &t, -51.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    arm2(&det, &seq, &t, -30.0f);
    for (int i = 0; i < 4; ++i) {
        feed(&det, &seq, &t, -48.0f);
        EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
        EXPECT(det.exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
        feed(&det, &seq, &t, -44.0f);
        EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    }
    EXPECT(det.emissions_end == 0);
    feed(&det, &seq, &t, -51.0f);
    feed(&det, &seq, &t, -51.0f);
    dbmap_activity_emission_t e = feed(&det, &seq, &t, -51.0f);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(e.end_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE);
}

/** Sustained loud above enter must not END via A3. */
static void test_sustained_loud_no_a3(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed(&det, &seq, &t, -51.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    arm2(&det, &seq, &t, -30.0f);
    for (int i = 0; i < 25; ++i) {
        dbmap_activity_emission_t e = feed(&det, &seq, &t, -35.0f);
        EXPECT(e.kind != DBMAP_ACTIVITY_EMIT_END);
        EXPECT(!det.secondary_ambient_valid);
        EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    }
}

/** Diag distinguishes secondary fields. */
static void test_a3_diag_snapshot(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = a3_cfg();
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed(&det, &seq, &t, -70.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    arm2(&det, &seq, &t, -30.0f);
    for (int i = 0; i < 5; ++i) {
        feed(&det, &seq, &t, -50.0f);
    }
    dbmap_activity_diag_t diag;
    dbmap_activity_detector_fill_diag(&det, &diag);
    EXPECT(diag.secondary_ambient_enabled);
    EXPECT(diag.secondary_ambient_valid);
    EXPECT(fabsf(diag.secondary_ambient_dbfs - (-50.0f)) < 0.01f);
    EXPECT(fabsf(diag.sec_exit_dbfs - (-46.0f)) < 0.01f);
    EXPECT(fabsf(diag.sec_cancel_dbfs - (-44.5f)) < 0.01f);
    EXPECT(fabsf(diag.frozen_baseline_dbfs - (-70.0f)) < 0.01f);
    char buf[512];
    EXPECT(dbmap_activity_format_rel_diag(&diag, buf, sizeof(buf)) > 0);
    EXPECT(strstr(buf, "sec=1") != NULL);
    EXPECT(strstr(buf, "sec_base=-50.00") != NULL);
    EXPECT(strcmp(dbmap_activity_exit_latch_name(DBMAP_ACTIVITY_EXIT_LATCH_SECONDARY), "secondary")
           == 0);
}

int main(void)
{
    test_e7_quiet_baseline_raised_ambient();
    test_e7_v1_reference_stays_open();
    test_t12_sudden_ambient_step();
    test_t13_long_quiet_baseline_episode();
    test_gradual_decay_to_m50_plateau();
    test_sustained_m50_after_strong_ends_via_a3();
    test_a3_cancel_then_renewed_activity();
    test_secondary_latch_oscillation_cancel_then_end();
    test_t15_weak_then_spike_no_premature_a3_end();
    test_primary_relative_wins_when_both_entry_qualify();
    test_stability_range_outside();
    test_stability_range_inside();
    test_jump_threshold_boundaries();
    test_stale_baseline_a3_inactive();
    test_primary_relative_unchanged_ear001();
    test_absolute_deep_silence_unchanged();
    test_relative_oscillation_preserved();
    test_sustained_loud_no_a3();
    test_a3_diag_snapshot();

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
