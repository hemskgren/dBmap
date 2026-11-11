/**
 * Focused offline A/B comparison of Relative END candidates vs defaults.
 *
 *   A: contrast=3, margin=5, hyst=1.5, stale=30s
 *   B: contrast=4, margin=5, hyst=1.5, stale=30s
 *   D: contrast=5, margin=4, hyst=1.5, stale=15s  (current defaults)
 *
 * Real activity_detector.c only. No max_episode overlay. No production changes.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     firmware/ear_components/ear_observation/activity_detector.c \
 *     firmware/ear_components/ear_observation/test_host/test_relative_end_ab_compare.c \
 *     -lm -o /tmp/test_relative_end_ab_compare && /tmp/test_relative_end_ab_compare
 */
#include "dbmap_activity_detector.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef enum { CFG_D = 0, CFG_A = 1, CFG_B = 2, CFG_N = 3 } cfg_id_t;

typedef struct {
    const char *name;
    float contrast;
    float margin;
    float hyst;
    uint32_t stale_ms;
} cand_t;

static const cand_t CAND[CFG_N] = {
    {"D(default)", 5.0f, 4.0f, 1.5f, 15000},
    {"A", 3.0f, 5.0f, 1.5f, 30000},
    {"B", 4.0f, 5.0f, 1.5f, 30000},
};

typedef struct {
    int armed; /* after first two episode windows */
    float contrast_meas;
    int ended;
    int path_rel; /* 1 relative, 0 absolute, -1 none */
    int64_t end_lat_ms; /* first PENDING_EXIT → END; -1 if n/a */
    int64_t episode_ms;
    uint32_t cancels;
    uint32_t pend_enters;
    int premature; /* END during intended-active phase */
} res_t;

static dbmap_activity_config_t make_cfg(const cand_t *c)
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
    cfg.min_event_contrast_db = c->contrast;
    cfg.return_margin_db = c->margin;
    cfg.cancel_hysteresis_db = c->hyst;
    cfg.relative_exit_hold_ms = 2000;
    cfg.baseline_stale_ms = c->stale_ms;
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
    }
}

static void start_episode(
    dbmap_activity_detector_t *det,
    uint32_t *seq,
    int64_t *t,
    float loud
)
{
    feed(det, seq, t, loud); /* PENDING_ENTER */
    feed(det, seq, t, loud); /* START */
}

static void res_clear(res_t *r)
{
    memset(r, 0, sizeof(*r));
    r->path_rel = -1;
    r->end_lat_ms = -1;
}

static void note_end(
    res_t *r,
    dbmap_activity_detector_t *det,
    dbmap_activity_exit_latch_t latch_before,
    int64_t first_pend_us,
    int64_t t,
    int premature
)
{
    r->ended = 1;
    r->premature = premature;
    r->path_rel = (latch_before == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE) ? 1 : 0;
    if (det->episode_start_us > 0) {
        r->episode_ms = (t - det->episode_start_us) / 1000;
    }
    if (first_pend_us > 0) {
        r->end_lat_ms = (t - first_pend_us) / 1000;
    }
    r->cancels = det->pending_exit_cancels;
    r->pend_enters = det->pending_exit_enters;
}

/**
 * Drive loud episode windows then optional settle sequence.
 * Records arming after the second episode window (START counts as first).
 */
static void run_loud_then_tail(
    const cand_t *cand,
    float baseline,
    float loud1,
    float loud2,
    const float *tail,
    int tail_n,
    int premature_until_idx, /* END before this tail index => premature; -1 = none */
    int64_t idle_gap_us,
    res_t *out
)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = make_cfg(cand);
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    int64_t first_pend_us = 0;
    res_clear(out);

    seed(&det, &seq, &t, baseline, 3);
    if (idle_gap_us > 0) {
        t += idle_gap_us;
    }
    start_episode(&det, &seq, &t, loud1);
    /* Second episode window (arming completes here). */
    {
        dbmap_activity_exit_latch_t lb = det.exit_latch;
        dbmap_activity_state_t sb = det.state;
        dbmap_activity_emission_t e = feed(&det, &seq, &t, loud2);
        out->armed = det.relative_armed ? 1 : 0;
        out->contrast_meas = det.measured_contrast_db;
        if (det.state == DBMAP_ACTIVITY_PENDING_EXIT && first_pend_us == 0) {
            first_pend_us = t;
        }
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            note_end(out, &det, sb == DBMAP_ACTIVITY_PENDING_EXIT ? lb : lb, first_pend_us, t, 1);
            return;
        }
    }

    for (int i = 0; i < tail_n; ++i) {
        dbmap_activity_exit_latch_t lb = det.exit_latch;
        dbmap_activity_state_t sb = det.state;
        dbmap_activity_emission_t e = feed(&det, &seq, &t, tail[i]);
        if (det.state == DBMAP_ACTIVITY_PENDING_EXIT && first_pend_us == 0) {
            first_pend_us = t;
        }
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            int prem = (premature_until_idx >= 0 && i < premature_until_idx) ? 1 : 0;
            note_end(
                out,
                &det,
                sb == DBMAP_ACTIVITY_PENDING_EXIT ? lb : det.exit_latch,
                first_pend_us,
                t,
                prem
            );
            /* latch cleared after END — prefer latch before feed */
            if (sb == DBMAP_ACTIVITY_PENDING_EXIT) {
                out->path_rel = (lb == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE) ? 1 : 0;
            }
            out->cancels = det.pending_exit_cancels;
            out->pend_enters = det.pending_exit_enters;
            return;
        }
    }
    out->cancels = det.pending_exit_cancels;
    out->pend_enters = det.pending_exit_enters;
    if (det.episode_start_us > 0) {
        out->episode_ms = (t - det.episode_start_us) / 1000;
    }
}

/* ---- case runners ------------------------------------------------------ */

typedef void (*case_fn)(const cand_t *c, res_t *out);

/** Expect arming decision from contrast = loud - baseline vs threshold. */
static void case_contrast_boundary(
    const cand_t *c,
    float baseline,
    float loud,
    const float *settle,
    int settle_n,
    res_t *out
)
{
    run_loud_then_tail(c, baseline, loud, loud, settle, settle_n, -1, 0, out);
}

static void C01_contrast_below_3(const cand_t *c, res_t *o)
{
    /* Must still meet enter (-45). baseline -47.5 + loud -45 → contrast 2.5. */
    const float settle[] = {-47.5f, -47.5f, -47.5f, -47.5f, -47.5f, -47.5f};
    case_contrast_boundary(c, -47.5f, -45.0f, settle, 6, o);
}

static void C02_contrast_at_3(const cand_t *c, res_t *o)
{
    /* -48 → -45 = contrast 3.0 */
    const float settle[] = {-48, -48, -48, -48, -48, -48};
    case_contrast_boundary(c, -48.0f, -45.0f, settle, 6, o);
}

static void C03_contrast_at_4(const cand_t *c, res_t *o)
{
    /* -48 → -44 = contrast 4.0 */
    const float settle[] = {-48, -48, -48, -48, -48, -48};
    case_contrast_boundary(c, -48.0f, -44.0f, settle, 6, o);
}

static void C04_contrast_at_5(const cand_t *c, res_t *o)
{
    /* -48 → -43 = contrast 5.0 */
    const float settle[] = {-48, -48, -48, -48, -48, -48};
    case_contrast_boundary(c, -48.0f, -43.0f, settle, 6, o);
}

static void C05_contrast_above_5(const cand_t *c, res_t *o)
{
    /* -51 → -30 = contrast 21 */
    const float settle[] = {-51, -51, -51, -51};
    case_contrast_boundary(c, -51.0f, -30.0f, settle, 4, o);
}

static void C06_weak_base_m60(const cand_t *c, res_t *o)
{
    /* quiet baseline, weak enter -44 → contrast 16 — arms all; settle -50 may not END */
    const float settle[] = {
        -50, -50, -50, -50, -50, -50, -50, -50, -50, -50, -50, -50
    };
    case_contrast_boundary(c, -60.0f, -44.0f, settle, 12, o);
}

static void C07_weak_base_m51(const cand_t *c, res_t *o)
{
    /* EAR-001-like: -51 base, -44 event → contrast 7 — arms all; settle ambient END */
    const float settle[] = {-51, -51, -51, -51};
    case_contrast_boundary(c, -51.0f, -44.0f, settle, 4, o);
}

static void C08_weak_base_m48(const cand_t *c, res_t *o)
{
    /* -48 base, -44 → contrast 4 */
    const float settle[] = {-48, -48, -48, -48, -48, -48};
    case_contrast_boundary(c, -48.0f, -44.0f, settle, 6, o);
}

static void C09_plateau_m44(const cand_t *c, res_t *o)
{
    /* After strong event, plateau at -44 (near enter). Cancel band depends on margin. */
    float tail[16];
    for (int i = 0; i < 16; ++i) {
        tail[i] = -44.0f;
    }
    case_contrast_boundary(c, -51.0f, -30.0f, tail, 16, o);
}

static void C10_plateau_m45(const cand_t *c, res_t *o)
{
    float tail[16];
    for (int i = 0; i < 16; ++i) {
        tail[i] = -45.0f;
    }
    case_contrast_boundary(c, -51.0f, -30.0f, tail, 16, o);
}

static void C11_plateau_m46(const cand_t *c, res_t *o)
{
    float tail[16];
    for (int i = 0; i < 16; ++i) {
        tail[i] = -46.0f;
    }
    case_contrast_boundary(c, -51.0f, -30.0f, tail, 16, o);
}

static void C12_oscillate_cancel(const cand_t *c, res_t *o)
{
    /*
     * After arming at base -51:
     * D: entry -47, cancel > -45.5 → bump -44 cancels
     * A/B: entry -46, cancel > -44.5 → bump -44 cancels (barely)
     * Oscillate 8 cycles then settle — premature if END before settle.
     */
    float tail[24];
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        tail[n++] = -48.0f;
        tail[n++] = -44.0f;
    }
    int settle_start = n;
    tail[n++] = -51.0f;
    tail[n++] = -51.0f;
    tail[n++] = -51.0f;
    tail[n++] = -51.0f;
    run_loud_then_tail(c, -51.0f, -30.0f, -30.0f, tail, n, settle_start, 0, o);
}

static void C13_oscillate_tighter(const cand_t *c, res_t *o)
{
    /* Quieter bump -45.0: may stay or cancel depending on cancel threshold. */
    float tail[24];
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        tail[n++] = -48.0f;
        tail[n++] = -45.0f;
    }
    int settle_start = n;
    for (int i = 0; i < 4; ++i) {
        tail[n++] = -51.0f;
    }
    run_loud_then_tail(c, -51.0f, -30.0f, -30.0f, tail, n, settle_start, 0, o);
}

static void C14_idle_gap_10s(const cand_t *c, res_t *o)
{
    const float settle[] = {-51, -51, -51, -51};
    run_loud_then_tail(c, -51.0f, -30.0f, -30.0f, settle, 4, -1, 10000000LL, o);
}

static void C15_idle_gap_20s(const cand_t *c, res_t *o)
{
    const float settle[] = {-51, -51, -51, -51, -51, -51, -51, -51};
    run_loud_then_tail(c, -51.0f, -30.0f, -30.0f, settle, 8, -1, 20000000LL, o);
}

static void C16_idle_gap_45s(const cand_t *c, res_t *o)
{
    const float settle[] = {-51, -51, -51, -51, -51, -51, -51, -51};
    run_loud_then_tail(c, -51.0f, -30.0f, -30.0f, settle, 8, -1, 45000000LL, o);
}

static void C17_gradual_decay(const cand_t *c, res_t *o)
{
    const float decay[] = {
        -32, -35, -38, -41, -44, -45, -46, -47, -48, -49, -50, -51, -51, -51, -51
    };
    case_contrast_boundary(c, -51.0f, -30.0f, decay, 15, o);
}

static void C18_intermittent_renew(const cand_t *c, res_t *o)
{
    /* 3× (1s quiet + 1s quiet + renew) then final quiet — premature during renew. */
    float tail[20];
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        tail[n++] = -51.0f;
        tail[n++] = -51.0f;
        tail[n++] = -35.0f;
    }
    int final_start = n;
    tail[n++] = -51.0f;
    tail[n++] = -51.0f;
    tail[n++] = -51.0f;
    tail[n++] = -51.0f;
    run_loud_then_tail(c, -51.0f, -30.0f, -30.0f, tail, n, final_start, 0, o);
}

static void C19_abs_path_deep_silence(const cand_t *c, res_t *o)
{
    /* Only 2 baseline samples → unreliable → absolute END at -60. */
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = make_cfg(c);
    dbmap_activity_detector_init(&det, &cfg);
    uint32_t seq = 0;
    int64_t t = 0;
    int64_t first_pend = 0;
    res_clear(o);
    seed(&det, &seq, &t, -51.0f, 2);
    start_episode(&det, &seq, &t, -30.0f);
    feed(&det, &seq, &t, -30.0f);
    o->armed = det.relative_armed ? 1 : 0;
    o->contrast_meas = det.measured_contrast_db;
    {
        dbmap_activity_exit_latch_t lb = det.exit_latch;
        dbmap_activity_state_t sb = det.state;
        dbmap_activity_emission_t e = feed(&det, &seq, &t, -60.0f);
        if (det.state == DBMAP_ACTIVITY_PENDING_EXIT) {
            first_pend = t;
        }
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            note_end(o, &det, sb == DBMAP_ACTIVITY_PENDING_EXIT ? lb : lb, first_pend, t, 0);
            return;
        }
    }
    {
        dbmap_activity_exit_latch_t lb = det.exit_latch;
        dbmap_activity_state_t sb = det.state;
        dbmap_activity_emission_t e = feed(&det, &seq, &t, -60.0f);
        if (first_pend == 0 && det.state == DBMAP_ACTIVITY_PENDING_EXIT) {
            first_pend = t;
        }
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            note_end(o, &det, sb == DBMAP_ACTIVITY_PENDING_EXIT ? lb : lb, first_pend, t, 0);
            if (sb == DBMAP_ACTIVITY_PENDING_EXIT) {
                o->path_rel = (lb == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE) ? 1 : 0;
            }
            return;
        }
    }
}

static void C20_sustained_loud_guard(const cand_t *c, res_t *o)
{
    float tail[25];
    for (int i = 0; i < 25; ++i) {
        tail[i] = -35.0f;
    }
    /* Any END here is premature false END. */
    run_loud_then_tail(c, -51.0f, -30.0f, -30.0f, tail, 25, 25, 0, o);
}

typedef struct {
    const char *id;
    const char *desc;
    case_fn fn;
    /**
     * Expected arming under physics contrast for each config:
     * use -1 = derive from measured contrast vs cand.contrast at runtime in printer.
     */
    int expect_end_if_armed; /* 1 if settle should natural-END when armed */
} case_def_t;

static const case_def_t CASES[] = {
    {"C01", "contrast 2.5 (base-47.5 loud-45)", C01_contrast_below_3, 1},
    {"C02", "contrast 3.0 boundary", C02_contrast_at_3, 1},
    {"C03", "contrast 4.0 boundary", C03_contrast_at_4, 1},
    {"C04", "contrast 5.0 boundary", C04_contrast_at_5, 1},
    {"C05", "strong contrast 21 (EAR-001-like)", C05_contrast_above_5, 1},
    {"C06", "weak evt base-60 settle-50", C06_weak_base_m60, 0},
    {"C07", "weak evt base-51 settle-51", C07_weak_base_m51, 1},
    {"C08", "weak evt base-48 settle-48", C08_weak_base_m48, 1},
    {"C09", "plateau -44 after strong", C09_plateau_m44, 0},
    {"C10", "plateau -45 after strong", C10_plateau_m45, 0},
    {"C11", "plateau -46 after strong", C11_plateau_m46, 1},
    {"C12", "oscillate -48/-44 then settle", C12_oscillate_cancel, 1},
    {"C13", "oscillate -48/-45 then settle", C13_oscillate_tighter, 1},
    {"C14", "idle gap 10s before START", C14_idle_gap_10s, 1},
    {"C15", "idle gap 20s before START", C15_idle_gap_20s, 1},
    {"C16", "idle gap 45s before START", C16_idle_gap_45s, 1},
    {"C17", "gradual decay to -51", C17_gradual_decay, 1},
    {"C18", "intermittent renew then END", C18_intermittent_renew, 1},
    {"C19", "absolute deep silence (unreliable base)", C19_abs_path_deep_silence, 0},
    {"C20", "sustained loud guard (must not END)", C20_sustained_loud_guard, 0},
};

static void format_cell(const res_t *r, char *buf, size_t buflen)
{
    if (r->premature) {
        snprintf(
            buf,
            buflen,
            "PREM/%s/%lld",
            r->path_rel ? "R" : "A",
            (long long)r->end_lat_ms
        );
        return;
    }
    if (!r->ended) {
        snprintf(
            buf,
            buflen,
            "open arm=%d c=%.1f",
            r->armed,
            (double)r->contrast_meas
        );
        return;
    }
    snprintf(
        buf,
        buflen,
        "END %s lat=%lld arm=%d",
        r->path_rel ? "REL" : "ABS",
        (long long)r->end_lat_ms,
        r->armed
    );
}

static int expect_armed(const cand_t *c, float contrast_meas, int baseline_ok)
{
    if (!baseline_ok) {
        return 0;
    }
    return contrast_meas + 1e-4f >= c->contrast;
}

int main(void)
{
    puts("Relative END A/B focused comparison (no max_episode)");
    puts("D=defaults(5/4/1.5/15s)  A=(3/5/1.5/30s)  B=(4/5/1.5/30s)");
    puts("");

    res_t results[sizeof(CASES) / sizeof(CASES[0])][CFG_N];
    memset(results, 0, sizeof(results));

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); ++i) {
        for (int k = 0; k < CFG_N; ++k) {
            CASES[i].fn(&CAND[k], &results[i][k]);
        }
    }

    printf("%-4s %-38s | %-28s | %-28s | %-28s\n", "ID", "case", "D", "A", "B");
    puts("-----+---------------------------------------+------------------------------+------------------------------+------------------------------");

    int prem_a = 0, prem_b = 0, prem_d = 0;
    int arm_mismatch_a = 0, arm_mismatch_b = 0, arm_mismatch_d = 0;
    int a_closes_d_open = 0, b_closes_d_open = 0;
    int a_opens_d_end = 0, b_opens_d_end = 0;

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); ++i) {
        char cd[32], ca[32], cb[32];
        format_cell(&results[i][CFG_D], cd, sizeof(cd));
        format_cell(&results[i][CFG_A], ca, sizeof(ca));
        format_cell(&results[i][CFG_B], cb, sizeof(cb));
        printf(
            "%-4s %-38s | %-28s | %-28s | %-28s\n",
            CASES[i].id,
            CASES[i].desc,
            cd,
            ca,
            cb
        );

        for (int k = 0; k < CFG_N; ++k) {
            const res_t *r = &results[i][k];
            /* baseline reliability inferred: contrast_meas valid when frozen reliable
             * — for C19 unreliable, armed must be 0. */
            int baseline_ok = 1;
            if (strcmp(CASES[i].id, "C19") == 0) {
                baseline_ok = 0;
            }
            if (strcmp(CASES[i].id, "C15") == 0 || strcmp(CASES[i].id, "C16") == 0) {
                /* stale may drop baseline depending on cand.stale_ms */
                baseline_ok = r->armed || r->contrast_meas > 0.0f;
                /* Better: treat expected arm from whether freeze survived.
                 * If contrast_meas==0 and !armed after loud episode, unreliable. */
                if (!r->armed && r->contrast_meas == 0.0f) {
                    baseline_ok = 0;
                } else if (r->armed) {
                    baseline_ok = 1;
                }
            }
            int exp = expect_armed(&CAND[k], r->contrast_meas, baseline_ok);
            /* For stale-drop cases contrast_meas may be 0 — expected unarmed. */
            if (strcmp(CASES[i].id, "C15") == 0) {
                /* 20s gap: D stale15 → unarmed; A/B stale30 → armed if loud */
                exp = (k == CFG_D) ? 0 : 1;
            }
            if (strcmp(CASES[i].id, "C16") == 0) {
                /* 45s gap: all stale windows drop → unarmed */
                exp = 0;
            }
            if (strcmp(CASES[i].id, "C19") == 0) {
                exp = 0;
            }
            if (r->armed != exp) {
                if (k == CFG_A) {
                    arm_mismatch_a++;
                }
                if (k == CFG_B) {
                    arm_mismatch_b++;
                }
                if (k == CFG_D) {
                    arm_mismatch_d++;
                }
            }
            if (r->premature) {
                if (k == CFG_A) {
                    prem_a++;
                }
                if (k == CFG_B) {
                    prem_b++;
                }
                if (k == CFG_D) {
                    prem_d++;
                }
            }
        }
        if (!results[i][CFG_D].ended && results[i][CFG_A].ended && !results[i][CFG_A].premature) {
            a_closes_d_open++;
        }
        if (!results[i][CFG_D].ended && results[i][CFG_B].ended && !results[i][CFG_B].premature) {
            b_closes_d_open++;
        }
        if (results[i][CFG_D].ended && !results[i][CFG_A].ended) {
            a_opens_d_end++;
        }
        if (results[i][CFG_D].ended && !results[i][CFG_B].ended) {
            b_opens_d_end++;
        }
    }

    puts("");
    puts("=== Arming detail (measured contrast / armed) ===");
    printf("%-4s %8s %6s %6s %6s  %8s %6s %6s %6s\n",
           "ID", "c_meas", "D", "A", "B", "expD/A/B", "okD", "okA", "okB");
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); ++i) {
        float cm = results[i][CFG_A].contrast_meas;
        if (cm == 0.0f) {
            cm = results[i][CFG_D].contrast_meas;
        }
        int exp[3];
        for (int k = 0; k < 3; ++k) {
            if (strcmp(CASES[i].id, "C19") == 0 || strcmp(CASES[i].id, "C16") == 0) {
                exp[k] = 0;
            } else if (strcmp(CASES[i].id, "C15") == 0) {
                exp[k] = (k == CFG_D) ? 0 : 1;
            } else {
                /* use each config's measured contrast (should match) */
                float m = results[i][k].contrast_meas;
                int base_ok = !(m == 0.0f && !results[i][k].armed
                                && (strcmp(CASES[i].id, "C15") == 0
                                    || strcmp(CASES[i].id, "C16") == 0));
                if (m == 0.0f && !results[i][k].armed
                    && (strcmp(CASES[i].id, "C15") == 0 || strcmp(CASES[i].id, "C16") == 0
                        || strcmp(CASES[i].id, "C19") == 0)) {
                    base_ok = 0;
                }
                /* Normal cases: baseline reliable */
                if (strcmp(CASES[i].id, "C01") == 0 || strcmp(CASES[i].id, "C02") == 0
                    || strcmp(CASES[i].id, "C03") == 0 || strcmp(CASES[i].id, "C04") == 0
                    || strcmp(CASES[i].id, "C05") == 0 || strcmp(CASES[i].id, "C06") == 0
                    || strcmp(CASES[i].id, "C07") == 0 || strcmp(CASES[i].id, "C08") == 0
                    || strcmp(CASES[i].id, "C09") == 0 || strcmp(CASES[i].id, "C10") == 0
                    || strcmp(CASES[i].id, "C11") == 0 || strcmp(CASES[i].id, "C12") == 0
                    || strcmp(CASES[i].id, "C13") == 0 || strcmp(CASES[i].id, "C14") == 0
                    || strcmp(CASES[i].id, "C17") == 0 || strcmp(CASES[i].id, "C18") == 0
                    || strcmp(CASES[i].id, "C20") == 0) {
                    base_ok = 1;
                    m = results[i][k].contrast_meas;
                }
                exp[k] = expect_armed(&CAND[k], m, base_ok);
            }
        }
        printf(
            "%-4s %8.2f %6d %6d %6d  %d/%d/%d      %6d %6d %6d\n",
            CASES[i].id,
            (double)cm,
            results[i][CFG_D].armed,
            results[i][CFG_A].armed,
            results[i][CFG_B].armed,
            exp[0],
            exp[1],
            exp[2],
            results[i][CFG_D].armed == exp[0],
            results[i][CFG_A].armed == exp[1],
            results[i][CFG_B].armed == exp[2]
        );
    }

    puts("");
    puts("=== Diff vs default (non-premature) ===");
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); ++i) {
        const res_t *d = &results[i][CFG_D];
        const res_t *a = &results[i][CFG_A];
        const res_t *b = &results[i][CFG_B];
        char da[48] = "=";
        char db[48] = "=";
        if (a->premature) {
            snprintf(da, sizeof(da), "PREMATURE");
        } else if (!d->ended && a->ended) {
            snprintf(da, sizeof(da), "NEW_END %s lat=%lld", a->path_rel ? "REL" : "ABS", (long long)a->end_lat_ms);
        } else if (d->ended && !a->ended) {
            snprintf(da, sizeof(da), "LOST_END");
        } else if (d->ended && a->ended && d->path_rel != a->path_rel) {
            snprintf(da, sizeof(da), "PATH_CHG");
        } else if (d->ended && a->ended && d->end_lat_ms != a->end_lat_ms) {
            snprintf(da, sizeof(da), "lat %lld→%lld", (long long)d->end_lat_ms, (long long)a->end_lat_ms);
        }
        if (b->premature) {
            snprintf(db, sizeof(db), "PREMATURE");
        } else if (!d->ended && b->ended) {
            snprintf(db, sizeof(db), "NEW_END %s lat=%lld", b->path_rel ? "REL" : "ABS", (long long)b->end_lat_ms);
        } else if (d->ended && !b->ended) {
            snprintf(db, sizeof(db), "LOST_END");
        } else if (d->ended && b->ended && d->path_rel != b->path_rel) {
            snprintf(db, sizeof(db), "PATH_CHG");
        } else if (d->ended && b->ended && d->end_lat_ms != b->end_lat_ms) {
            snprintf(db, sizeof(db), "lat %lld→%lld", (long long)d->end_lat_ms, (long long)b->end_lat_ms);
        }
        if (strcmp(da, "=") != 0 || strcmp(db, "=") != 0) {
            printf("%-4s  A:%-24s  B:%-24s  (%s)\n", CASES[i].id, da, db, CASES[i].desc);
        }
    }

    printf(
        "\nTotals: premature D/A/B=%d/%d/%d  arm_mismatch D/A/B=%d/%d/%d\n"
        "closes_default_open A=%d B=%d  lost_default_end A=%d B=%d\n",
        prem_d,
        prem_a,
        prem_b,
        arm_mismatch_d,
        arm_mismatch_a,
        arm_mismatch_b,
        a_closes_d_open,
        b_closes_d_open,
        a_opens_d_end,
        b_opens_d_end
    );

    /* Explicit safety gates for recommendation */
    puts("\n=== Safety gates ===");
    int gate_ok_a = (prem_a == 0 && results[19][CFG_A].ended == 0);
    int gate_ok_b = (prem_b == 0 && results[19][CFG_B].ended == 0);
    printf("A: no premature + C20 no END → %s\n", gate_ok_a ? "PASS" : "FAIL");
    printf("B: no premature + C20 no END → %s\n", gate_ok_b ? "PASS" : "FAIL");
    printf(
        "C19 absolute path preserved D/A/B ended=%d/%d/%d path_rel=%d/%d/%d\n",
        results[18][CFG_D].ended,
        results[18][CFG_A].ended,
        results[18][CFG_B].ended,
        results[18][CFG_D].path_rel,
        results[18][CFG_A].path_rel,
        results[18][CFG_B].path_rel
    );

    return 0;
}
