/**
 * Offline Relative END parameter sensitivity study (real activity_detector.c).
 *
 * Reuses E1–E10 signal sequences from the long-running experiment suite.
 * Does not change production detector logic.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     firmware/ear_components/ear_observation/activity_detector.c \
 *     firmware/ear_components/ear_observation/test_host/test_relative_end_param_sweep.c \
 *     -lm -o /tmp/test_relative_end_param_sweep \
 *     && /tmp/test_relative_end_param_sweep
 *
 * max_episode_ms is NOT implemented in activity_detector (lifecycle v2 only,
 * currently disabled). This harness evaluates it as an explicit post-START
 * hard-cap overlay on top of the real detector feed, labeled separately.
 */
#include "dbmap_activity_detector.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    END_NONE = 0,
    END_RELATIVE,
    END_ABSOLUTE,
    END_MAX_EPISODE, /* overlay only */
    END_OTHER
} end_path_t;

typedef struct {
    int ended;
    end_path_t path;
    int64_t episode_dur_ms;
    int64_t exit_latency_ms; /* END time − first PENDING_EXIT entry; -1 if n/a */
    int64_t first_pending_at_ms;
    uint32_t cancels;
    uint32_t pend_enters;
    int premature; /* END during intended-active oscillation/renew window */
    int armed_after_arm2;
    int baseline_reliable;
    dbmap_activity_exit_latch_t latch_at_end;
} run_result_t;

typedef struct {
    const char *label;
    float contrast;
    float margin;
    float hyst;
    uint32_t stale_ms;
    uint32_t max_episode_ms; /* 0 = disabled overlay (current production) */
} sweep_cfg_t;

static dbmap_activity_config_t make_cfg(const sweep_cfg_t *s)
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
    cfg.return_margin_db = s->margin;
    cfg.cancel_hysteresis_db = s->hyst;
    cfg.relative_exit_hold_ms = 2000;
    cfg.min_event_contrast_db = s->contrast;
    cfg.baseline_stale_ms = s->stale_ms;
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
        feed(det, *seq, *t, rms);
    }
}

static void start_loud(
    dbmap_activity_detector_t *det,
    uint32_t *seq,
    int64_t *t,
    float loud
)
{
    *seq += 1;
    *t += 1000000;
    feed(det, *seq, *t, loud);
    *seq += 1;
    *t += 1000000;
    feed(det, *seq, *t, loud);
}

static void result_init(run_result_t *r)
{
    memset(r, 0, sizeof(*r));
    r->exit_latency_ms = -1;
    r->first_pending_at_ms = -1;
    r->latch_at_end = DBMAP_ACTIVITY_EXIT_LATCH_NONE;
}

static void note_pending(run_result_t *r, const dbmap_activity_detector_t *det, int64_t t)
{
    if (det->state == DBMAP_ACTIVITY_PENDING_EXIT && r->first_pending_at_ms < 0) {
        r->first_pending_at_ms = t / 1000;
    }
}

static int apply_max_episode_overlay(
    dbmap_activity_detector_t *det,
    const sweep_cfg_t *scfg,
    run_result_t *r,
    int64_t t
)
{
    if (scfg->max_episode_ms == 0 || r->ended) {
        return 0;
    }
    if (det->state != DBMAP_ACTIVITY_ACTIVE && det->state != DBMAP_ACTIVITY_PENDING_EXIT) {
        return 0;
    }
    if (det->episode_start_us <= 0) {
        return 0;
    }
    const int64_t age_ms = (t - det->episode_start_us) / 1000;
    if (age_ms < (int64_t)scfg->max_episode_ms) {
        return 0;
    }
    r->ended = 1;
    r->path = END_MAX_EPISODE;
    r->episode_dur_ms = age_ms;
    r->latch_at_end = det->exit_latch;
    if (r->first_pending_at_ms >= 0) {
        r->exit_latency_ms = (t / 1000) - r->first_pending_at_ms;
    }
    /* Force detector idle so subsequent feeds do not double-count. */
    (void)dbmap_activity_detector_force_end(det);
    return 1;
}

static void note_natural_end(
    run_result_t *r,
    int64_t episode_start_us,
    dbmap_activity_exit_latch_t latch_before_end,
    int64_t t
)
{
    r->ended = 1;
    r->latch_at_end = latch_before_end;
    if (episode_start_us > 0) {
        r->episode_dur_ms = (t - episode_start_us) / 1000;
    }
    if (latch_before_end == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE) {
        r->path = END_RELATIVE;
    } else if (latch_before_end == DBMAP_ACTIVITY_EXIT_LATCH_ABSOLUTE) {
        r->path = END_ABSOLUTE;
    } else {
        r->path = END_OTHER;
    }
    if (r->first_pending_at_ms >= 0) {
        r->exit_latency_ms = (t / 1000) - r->first_pending_at_ms;
    }
}

/** Capture latch used for an END that completes from PENDING_EXIT. */
static int absorb_end(
    run_result_t *r,
    const dbmap_activity_detector_t *det,
    dbmap_activity_state_t state_before,
    dbmap_activity_exit_latch_t latch_before,
    dbmap_activity_emission_t e,
    int64_t t
)
{
    if (e.kind != DBMAP_ACTIVITY_EMIT_END) {
        return 0;
    }
    dbmap_activity_exit_latch_t used = latch_before;
    if (state_before != DBMAP_ACTIVITY_PENDING_EXIT) {
        /* Same-call ACTIVE→…→END is not expected for natural holds ≥1s. */
        used = latch_before;
    }
    note_natural_end(r, det->episode_start_us, used, t);
    return 1;
}

static void finalize_open(run_result_t *r, const dbmap_activity_detector_t *det, int64_t t)
{
    r->cancels = det->pending_exit_cancels;
    r->pend_enters = det->pending_exit_enters;
    if (!r->ended && det->episode_start_us > 0) {
        r->episode_dur_ms = (t - det->episode_start_us) / 1000;
    } else if (!r->ended) {
        r->episode_dur_ms = 0;
    }
    r->cancels = det->pending_exit_cancels;
    r->pend_enters = det->pending_exit_enters;
}

/* ---- scenarios --------------------------------------------------------- */

static void run_e1(const sweep_cfg_t *scfg, run_result_t *r)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = make_cfg(scfg);
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    result_init(r);

    seed_baseline(&det, &seq, &t, -51.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -30.0f);
    r->armed_after_arm2 = det.relative_armed ? 1 : 0;
    r->baseline_reliable = det.baseline_reliable ? 1 : 0;

    const float steps[] = {
        -32.0f, -35.0f, -38.0f, -41.0f, -44.0f, -46.0f, -48.0f, -49.0f, -50.0f, -51.0f,
        -51.0f, -51.0f, -51.0f
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_exit_latch_t latch_before = det.exit_latch;
        dbmap_activity_state_t st_before = det.state;
        dbmap_activity_emission_t e = feed(&det, seq, t, steps[i]);
        note_pending(r, &det, t);
        if (absorb_end(r, &det, st_before, latch_before, e, t)) {
            break;
        }
        if (apply_max_episode_overlay(&det, scfg, r, t)) {
            break;
        }
    }
    finalize_open(r, &det, t);
}

static void run_e2(const sweep_cfg_t *scfg, run_result_t *r)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = make_cfg(scfg);
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    result_init(r);

    seed_baseline(&det, &seq, &t, -51.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -30.0f);
    r->armed_after_arm2 = det.relative_armed ? 1 : 0;
    r->baseline_reliable = det.baseline_reliable ? 1 : 0;

    /* Oscillation phase: premature if END here. */
    for (int i = 0; i < 8; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_exit_latch_t lb = det.exit_latch;
        dbmap_activity_state_t sb = det.state;
        dbmap_activity_emission_t e = feed(&det, seq, t, -48.0f);
        note_pending(r, &det, t);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            r->premature = 1;
            note_natural_end(r, det.episode_start_us, lb, t);
            finalize_open(r, &det, t);
            return;
        }
        if (apply_max_episode_overlay(&det, scfg, r, t)) {
            r->premature = 1;
            finalize_open(r, &det, t);
            return;
        }

        seq++;
        t += 1000000;
        lb = det.exit_latch;
        sb = det.state;
        e = feed(&det, seq, t, -44.0f);
        note_pending(r, &det, t);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            r->premature = 1;
            note_natural_end(r, det.episode_start_us, lb, t);
            finalize_open(r, &det, t);
            return;
        }
        if (apply_max_episode_overlay(&det, scfg, r, t)) {
            r->premature = 1;
            finalize_open(r, &det, t);
            return;
        }
    }

    for (int i = 0; i < 4; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_exit_latch_t lb = det.exit_latch;
        dbmap_activity_state_t sb = det.state;
        dbmap_activity_emission_t e = feed(&det, seq, t, -51.0f);
        note_pending(r, &det, t);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            note_natural_end(r, det.episode_start_us, lb, t);
            break;
        }
        if (apply_max_episode_overlay(&det, scfg, r, t)) {
            break;
        }
    }
    finalize_open(r, &det, t);
}

static void run_e3(const sweep_cfg_t *scfg, run_result_t *r)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = make_cfg(scfg);
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    result_init(r);

    seed_baseline(&det, &seq, &t, -48.0f, 3);
    start_loud(&det, &seq, &t, -44.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -44.0f);
    r->armed_after_arm2 = det.relative_armed ? 1 : 0;
    r->baseline_reliable = det.baseline_reliable ? 1 : 0;

    /* Extend tail so max_episode overlays (60s/120s) can fire. */
    for (int i = 0; i < 130; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_exit_latch_t lb = det.exit_latch;
        dbmap_activity_state_t sb = det.state;
        dbmap_activity_emission_t e = feed(&det, seq, t, -48.0f);
        note_pending(r, &det, t);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            note_natural_end(r, det.episode_start_us, lb, t);
            break;
        }
        if (apply_max_episode_overlay(&det, scfg, r, t)) {
            break;
        }
    }
    finalize_open(r, &det, t);
}

static void run_e4(const sweep_cfg_t *scfg, run_result_t *r)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = make_cfg(scfg);
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    result_init(r);

    seed_baseline(&det, &seq, &t, -51.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -30.0f);
    r->armed_after_arm2 = det.relative_armed ? 1 : 0;
    r->baseline_reliable = det.baseline_reliable ? 1 : 0;

    for (int cycle = 0; cycle < 3; ++cycle) {
        seq++;
        t += 1000000;
        dbmap_activity_exit_latch_t lb = det.exit_latch;
        dbmap_activity_state_t sb = det.state;
        dbmap_activity_emission_t e = feed(&det, seq, t, -51.0f);
        note_pending(r, &det, t);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            r->premature = 1;
            note_natural_end(r, det.episode_start_us, lb, t);
            finalize_open(r, &det, t);
            return;
        }
        if (apply_max_episode_overlay(&det, scfg, r, t)) {
            r->premature = 1;
            finalize_open(r, &det, t);
            return;
        }

        seq++;
        t += 1000000;
        lb = det.exit_latch;
        sb = det.state;
        e = feed(&det, seq, t, -51.0f);
        note_pending(r, &det, t);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            /* Hold reaches 2s on this second quiet window under rel hold —
             * with defaults hold is only 1s so far; END here during renew
             * protocol is premature relative to E4 intent (renew before END). */
            r->premature = 1;
            note_natural_end(r, det.episode_start_us, lb, t);
            finalize_open(r, &det, t);
            return;
        }
        if (apply_max_episode_overlay(&det, scfg, r, t)) {
            r->premature = 1;
            finalize_open(r, &det, t);
            return;
        }

        seq++;
        t += 1000000;
        lb = det.exit_latch;
        sb = det.state;
        e = feed(&det, seq, t, -35.0f);
        note_pending(r, &det, t);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            r->premature = 1;
            note_natural_end(r, det.episode_start_us, lb, t);
            finalize_open(r, &det, t);
            return;
        }
        if (apply_max_episode_overlay(&det, scfg, r, t)) {
            r->premature = 1;
            finalize_open(r, &det, t);
            return;
        }
    }

    for (int i = 0; i < 4; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_exit_latch_t lb = det.exit_latch;
        dbmap_activity_state_t sb = det.state;
        dbmap_activity_emission_t e = feed(&det, seq, t, -51.0f);
        note_pending(r, &det, t);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            note_natural_end(r, det.episode_start_us, lb, t);
            break;
        }
        if (apply_max_episode_overlay(&det, scfg, r, t)) {
            break;
        }
    }
    finalize_open(r, &det, t);
}

static void run_e5(const sweep_cfg_t *scfg, run_result_t *r)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = make_cfg(scfg);
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    result_init(r);

    seed_baseline(&det, &seq, &t, -51.0f, 3);
    t += 20000000; /* fixed 20 s gap (scenario definition) */

    start_loud(&det, &seq, &t, -30.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -30.0f);
    r->armed_after_arm2 = det.relative_armed ? 1 : 0;
    r->baseline_reliable = det.baseline_reliable ? 1 : 0;

    for (int i = 0; i < 130; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_exit_latch_t lb = det.exit_latch;
        dbmap_activity_state_t sb = det.state;
        dbmap_activity_emission_t e = feed(&det, seq, t, -51.0f);
        note_pending(r, &det, t);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            note_natural_end(r, det.episode_start_us, lb, t);
            break;
        }
        if (apply_max_episode_overlay(&det, scfg, r, t)) {
            break;
        }
    }
    finalize_open(r, &det, t);
}

static void run_e6(const sweep_cfg_t *scfg, run_result_t *r)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = make_cfg(scfg);
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    result_init(r);

    seed_baseline(&det, &seq, &t, -51.0f, 2);
    start_loud(&det, &seq, &t, -30.0f);
    r->baseline_reliable = det.baseline_reliable ? 1 : 0;
    seq++;
    t += 1000000;
    feed(&det, seq, t, -30.0f);
    r->armed_after_arm2 = det.relative_armed ? 1 : 0;

    seq++;
    t += 1000000;
    dbmap_activity_exit_latch_t lb = det.exit_latch;
    dbmap_activity_state_t sb = det.state;
    dbmap_activity_emission_t e = feed(&det, seq, t, -60.0f);
    note_pending(r, &det, t);
    if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
        note_natural_end(r, det.episode_start_us, lb, t);
        finalize_open(r, &det, t);
        return;
    }

    seq++;
    t += 1000000;
    lb = det.exit_latch;
    sb = det.state;
    e = feed(&det, seq, t, -60.0f);
    note_pending(r, &det, t);
    if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
        note_natural_end(r, det.episode_start_us, lb, t);
    } else {
        (void)apply_max_episode_overlay(&det, scfg, r, t);
    }
    finalize_open(r, &det, t);
}

static void run_e7(const sweep_cfg_t *scfg, run_result_t *r)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = make_cfg(scfg);
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    result_init(r);

    seed_baseline(&det, &seq, &t, -70.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -30.0f);
    r->armed_after_arm2 = det.relative_armed ? 1 : 0;
    r->baseline_reliable = det.baseline_reliable ? 1 : 0;

    for (int i = 0; i < 130; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_exit_latch_t lb = det.exit_latch;
        dbmap_activity_state_t sb = det.state;
        dbmap_activity_emission_t e = feed(&det, seq, t, -50.0f);
        note_pending(r, &det, t);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            note_natural_end(r, det.episode_start_us, lb, t);
            break;
        }
        if (apply_max_episode_overlay(&det, scfg, r, t)) {
            break;
        }
    }
    finalize_open(r, &det, t);
}

static void run_e8(const sweep_cfg_t *scfg, run_result_t *r)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = make_cfg(scfg);
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    result_init(r);

    seed_baseline(&det, &seq, &t, -48.0f, 3);
    start_loud(&det, &seq, &t, -44.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -44.0f);
    r->armed_after_arm2 = det.relative_armed ? 1 : 0;
    r->baseline_reliable = det.baseline_reliable ? 1 : 0;

    seq++;
    t += 1000000;
    feed(&det, seq, t, -25.0f);

    for (int i = 0; i < 130; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_exit_latch_t lb = det.exit_latch;
        dbmap_activity_state_t sb = det.state;
        dbmap_activity_emission_t e = feed(&det, seq, t, -48.0f);
        note_pending(r, &det, t);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            note_natural_end(r, det.episode_start_us, lb, t);
            break;
        }
        if (apply_max_episode_overlay(&det, scfg, r, t)) {
            break;
        }
    }
    finalize_open(r, &det, t);
}

static void run_e9(const sweep_cfg_t *scfg, run_result_t *r)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = make_cfg(scfg);
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    result_init(r);

    seed_baseline(&det, &seq, &t, -70.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -30.0f);
    r->armed_after_arm2 = det.relative_armed ? 1 : 0;
    r->baseline_reliable = det.baseline_reliable ? 1 : 0;

    seq++;
    t += 1000000;
    feed(&det, seq, t, -58.0f);
    note_pending(r, &det, t);

    seq++;
    t += 1000000;
    dbmap_activity_exit_latch_t lb = det.exit_latch;
    dbmap_activity_state_t sb = det.state;
    dbmap_activity_emission_t e = feed(&det, seq, t, -50.0f);
    note_pending(r, &det, t);
    if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
        /* Abs hold is 1s; this cancel window should not END under defaults. */
        r->premature = 1;
        note_natural_end(r, det.episode_start_us, lb, t);
        finalize_open(r, &det, t);
        return;
    }

    for (int i = 0; i < 130; ++i) {
        seq++;
        t += 1000000;
        lb = det.exit_latch;
        sb = det.state;
        e = feed(&det, seq, t, -50.0f);
        note_pending(r, &det, t);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            note_natural_end(r, det.episode_start_us, lb, t);
            break;
        }
        if (apply_max_episode_overlay(&det, scfg, r, t)) {
            break;
        }
    }
    finalize_open(r, &det, t);
}

static void run_e10(const sweep_cfg_t *scfg, run_result_t *r)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = make_cfg(scfg);
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    result_init(r);

    seed_baseline(&det, &seq, &t, -51.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -30.0f);
    r->armed_after_arm2 = det.relative_armed ? 1 : 0;
    r->baseline_reliable = det.baseline_reliable ? 1 : 0;

    for (int i = 0; i < 130; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_exit_latch_t lb = det.exit_latch;
        dbmap_activity_state_t sb = det.state;
        dbmap_activity_emission_t e = feed(&det, seq, t, -46.0f);
        note_pending(r, &det, t);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            note_natural_end(r, det.episode_start_us, lb, t);
            break;
        }
        if (apply_max_episode_overlay(&det, scfg, r, t)) {
            break;
        }
    }
    finalize_open(r, &det, t);
}

typedef void (*scenario_fn)(const sweep_cfg_t *, run_result_t *);

static const struct {
    const char *id;
    scenario_fn fn;
    int default_ends; /* reference expectation under current defaults */
} SCENARIOS[] = {
    {"E1", run_e1, 1},
    {"E2", run_e2, 1},
    {"E3", run_e3, 0},
    {"E4", run_e4, 1},
    {"E5", run_e5, 0},
    {"E6", run_e6, 1},
    {"E7", run_e7, 0},
    {"E8", run_e8, 0},
    {"E9", run_e9, 0},
    {"E10", run_e10, 0},
};

static const char *path_name(end_path_t p)
{
    switch (p) {
    case END_RELATIVE:
        return "REL";
    case END_ABSOLUTE:
        return "ABS";
    case END_MAX_EPISODE:
        return "MAX";
    case END_OTHER:
        return "OTH";
    case END_NONE:
    default:
        return "---";
    }
}

static void print_cfg_header(const sweep_cfg_t *s)
{
    printf(
        "\n## %s  contrast=%.1f margin=%.1f hyst=%.1f stale=%lu max_ep=%lu\n",
        s->label,
        (double)s->contrast,
        (double)s->margin,
        (double)s->hyst,
        (unsigned long)s->stale_ms,
        (unsigned long)s->max_episode_ms
    );
    printf(
        "sc  end path lat_ms dur_ms arm base canc prem | vs_default\n"
    );
}

static void run_matrix(const sweep_cfg_t *s, run_result_t *out /*[10]*/)
{
    for (size_t i = 0; i < sizeof(SCENARIOS) / sizeof(SCENARIOS[0]); ++i) {
        SCENARIOS[i].fn(s, &out[i]);
    }
}

static void print_matrix(const sweep_cfg_t *s, const run_result_t *ref, const run_result_t *cur)
{
    print_cfg_header(s);
    int newly_ended = 0;
    int newly_open = 0;
    int premature = 0;
    int path_regress = 0;

    for (size_t i = 0; i < sizeof(SCENARIOS) / sizeof(SCENARIOS[0]); ++i) {
        const run_result_t *r = &cur[i];
        const run_result_t *b = &ref[i];
        const char *delta = "=";
        if (r->ended && !b->ended) {
            delta = "NEW_END";
            newly_ended++;
        } else if (!r->ended && b->ended) {
            delta = "LOST_END";
            newly_open++;
        } else if (r->premature && !b->premature) {
            delta = "PREMATURE";
            premature++;
        } else if (r->ended && b->ended && r->path != b->path
                   && r->path != END_MAX_EPISODE && b->path != END_MAX_EPISODE) {
            delta = "PATH_CHG";
            path_regress++;
        } else if (r->premature) {
            delta = "prem";
            premature++;
        }

        printf(
            "%-3s %s %-4s %6lld %6lld  %d    %d  %4lu  %d  | %s\n",
            SCENARIOS[i].id,
            r->ended ? "Y" : "N",
            path_name(r->path),
            (long long)r->exit_latency_ms,
            (long long)r->episode_dur_ms,
            r->armed_after_arm2,
            r->baseline_reliable,
            (unsigned long)r->cancels,
            r->premature,
            delta
        );
    }
    printf(
        "summary: newly_ended=%d lost_end=%d premature=%d path_chg=%d\n",
        newly_ended,
        newly_open,
        premature,
        path_regress
    );
}

/* Guard: sustained loud activity must not END (false END check). */
static int guard_sustained_loud(const sweep_cfg_t *scfg)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = make_cfg(scfg);
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    seed_baseline(&det, &seq, &t, -51.0f, 3);
    start_loud(&det, &seq, &t, -30.0f);
    seq++;
    t += 1000000;
    feed(&det, seq, t, -30.0f);
    for (int i = 0; i < 20; ++i) {
        seq++;
        t += 1000000;
        dbmap_activity_emission_t e = feed(&det, seq, t, -35.0f);
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            return 1; /* false END */
        }
        if (scfg->max_episode_ms > 0) {
            const int64_t age = (t - det.episode_start_us) / 1000;
            if (age >= (int64_t)scfg->max_episode_ms) {
                /* Cap firing during loud activity is expected for max_episode
                 * policy — not counted as relative false END. */
                return 0;
            }
        }
    }
    return 0;
}

int main(void)
{
    puts("Relative END parameter sensitivity (activity_detector.c)");
    puts("Reference defaults: contrast=5 margin=4 hyst=1.5 stale=15000 max_ep=0");
    puts("exit_latency_ms = time from first PENDING_EXIT entry to END (-1 if never)");
    puts("NEW_END on previously open scenarios is a benefit only if premature=0");
    puts("max_ep overlay is lifecycle-v2 hard-cap policy, not Relative END v1");

    const sweep_cfg_t defaults = {
        .label = "DEFAULT",
        .contrast = 5.0f,
        .margin = 4.0f,
        .hyst = 1.5f,
        .stale_ms = 15000,
        .max_episode_ms = 0
    };

    run_result_t ref[10];
    run_matrix(&defaults, ref);
    print_matrix(&defaults, ref, ref);

    /* Independent one-at-a-time sweeps */
    const sweep_cfg_t independents[] = {
        {"contrast=3", 3.0f, 4.0f, 1.5f, 15000, 0},
        {"contrast=4", 4.0f, 4.0f, 1.5f, 15000, 0},
        {"contrast=5", 5.0f, 4.0f, 1.5f, 15000, 0},
        {"margin=4", 5.0f, 4.0f, 1.5f, 15000, 0},
        {"margin=5", 5.0f, 5.0f, 1.5f, 15000, 0},
        {"margin=6", 5.0f, 6.0f, 1.5f, 15000, 0},
        {"hyst=1.5", 5.0f, 4.0f, 1.5f, 15000, 0},
        {"hyst=2.0", 5.0f, 4.0f, 2.0f, 15000, 0},
        {"hyst=3.0", 5.0f, 4.0f, 3.0f, 15000, 0},
        {"stale=15s", 5.0f, 4.0f, 1.5f, 15000, 0},
        {"stale=30s", 5.0f, 4.0f, 1.5f, 30000, 0},
        {"stale=60s", 5.0f, 4.0f, 1.5f, 60000, 0},
        {"max_ep=off", 5.0f, 4.0f, 1.5f, 15000, 0},
        {"max_ep=60s", 5.0f, 4.0f, 1.5f, 15000, 60000},
        {"max_ep=120s", 5.0f, 4.0f, 1.5f, 15000, 120000},
        {"max_ep=300s", 5.0f, 4.0f, 1.5f, 15000, 300000},
    };

    puts("\n=== Independent sweeps ===");
    for (size_t i = 0; i < sizeof(independents) / sizeof(independents[0]); ++i) {
        /* Skip exact duplicates of default print except labeled once. */
        if (strcmp(independents[i].label, "contrast=5") == 0
            || strcmp(independents[i].label, "margin=4") == 0
            || strcmp(independents[i].label, "hyst=1.5") == 0
            || strcmp(independents[i].label, "stale=15s") == 0
            || strcmp(independents[i].label, "max_ep=off") == 0) {
            continue;
        }
        run_result_t cur[10];
        run_matrix(&independents[i], cur);
        print_matrix(&independents[i], ref, cur);
        if (guard_sustained_loud(&independents[i])) {
            printf("GUARD FAIL: sustained loud produced natural END\n");
        }
    }

    /* Meaningful combinations targeting open-ended cases without wild margins. */
    const sweep_cfg_t combos[] = {
        {"c3_m4_h1.5", 3.0f, 4.0f, 1.5f, 15000, 0},
        {"c3_m5_h1.5", 3.0f, 5.0f, 1.5f, 15000, 0},
        {"c3_m5_h1.5_s30", 3.0f, 5.0f, 1.5f, 30000, 0},
        {"c4_m5_h1.5", 4.0f, 5.0f, 1.5f, 15000, 0},
        {"c4_m5_h1.5_s30", 4.0f, 5.0f, 1.5f, 30000, 0},
        {"c3_stale30", 3.0f, 4.0f, 1.5f, 30000, 0},
        {"c3_m5_h2", 3.0f, 5.0f, 2.0f, 15000, 0},
        {"c4_m5_h2", 4.0f, 5.0f, 2.0f, 15000, 0},
        {"c4_m6_h2", 4.0f, 6.0f, 2.0f, 15000, 0},
        {"c5_m5_h2", 5.0f, 5.0f, 2.0f, 15000, 0},
        {"c5_m6_h3", 5.0f, 6.0f, 3.0f, 15000, 0},
        {"c3_stale30", 3.0f, 4.0f, 1.5f, 30000, 0},
        {"c3_m5_stale30", 3.0f, 5.0f, 2.0f, 30000, 0},
        {"defaults+max60", 5.0f, 4.0f, 1.5f, 15000, 60000},
        {"c3_m5_h2+max60", 3.0f, 5.0f, 2.0f, 15000, 60000},
        {"c3_m5_h2+max120", 3.0f, 5.0f, 2.0f, 15000, 120000},
    };

    puts("\n=== Combinations ===");
    for (size_t i = 0; i < sizeof(combos) / sizeof(combos[0]); ++i) {
        run_result_t cur[10];
        run_matrix(&combos[i], cur);
        print_matrix(&combos[i], ref, cur);
        if (guard_sustained_loud(&combos[i])) {
            printf("GUARD FAIL: sustained loud produced natural END\n");
        }
    }

    /* Compact benefit table for open scenarios under key configs */
    puts("\n=== Open-scenario closure matrix (E3,E5,E7,E8,E9,E10) ===");
    puts("config                         E3   E5   E7   E8   E9  E10  prem(E2/E4)");
    const sweep_cfg_t key[] = {
        defaults,
        {"contrast=3", 3.0f, 4.0f, 1.5f, 15000, 0},
        {"contrast=4", 4.0f, 4.0f, 1.5f, 15000, 0},
        {"margin=5", 5.0f, 5.0f, 1.5f, 15000, 0},
        {"margin=6", 5.0f, 6.0f, 1.5f, 15000, 0},
        {"hyst=2", 5.0f, 4.0f, 2.0f, 15000, 0},
        {"hyst=3", 5.0f, 4.0f, 3.0f, 15000, 0},
        {"stale=30s", 5.0f, 4.0f, 1.5f, 30000, 0},
        {"stale=60s", 5.0f, 4.0f, 1.5f, 60000, 0},
        {"c3_m5_h2", 3.0f, 5.0f, 2.0f, 15000, 0},
        {"c4_m6_h2", 4.0f, 6.0f, 2.0f, 15000, 0},
        {"max_ep=60s", 5.0f, 4.0f, 1.5f, 15000, 60000},
        {"max_ep=120s", 5.0f, 4.0f, 1.5f, 15000, 120000},
        {"c3_m5_h2+max60", 3.0f, 5.0f, 2.0f, 15000, 60000},
    };
    const int open_idx[] = {2, 4, 6, 7, 8, 9}; /* E3 E5 E7 E8 E9 E10 */

    for (size_t k = 0; k < sizeof(key) / sizeof(key[0]); ++k) {
        run_result_t cur[10];
        run_matrix(&key[k], cur);
        printf("%-28s", key[k].label);
        for (size_t j = 0; j < sizeof(open_idx) / sizeof(open_idx[0]); ++j) {
            const run_result_t *r = &cur[open_idx[j]];
            if (!r->ended) {
                printf("  -- ");
            } else {
                printf(" %s ", path_name(r->path));
            }
        }
        printf("  E2prem=%d E4prem=%d\n", cur[1].premature, cur[3].premature);
    }

    puts("\nDone.");
    return 0;
}
