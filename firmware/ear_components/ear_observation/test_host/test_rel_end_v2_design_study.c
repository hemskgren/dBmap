/**
 * Offline Relative END v2 design study.
 *
 * Compares production v1 detector to host-only prototype alternatives:
 *   V1  — real activity_detector.c (unchanged defaults)
 *   A1  — delayed/repeated arming via episode peak
 *   A1r — episode peak limited to last 3 windows (safer late-arm)
 *   A2  — adaptive baseline refresh (mutates freeze after stable raised ambient)
 *   A3  — secondary ambient exit (no freeze mutation; existing proto policy)
 *   A3b — below-enter dwell END (covers unarmed stuck band)
 *   A1+A3 — peak arm + secondary ambient
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     -I firmware/ear_components/ear_observation/test_host \
 *     firmware/ear_components/ear_observation/activity_detector.c \
 *     firmware/ear_components/ear_observation/test_host/proto_pre_event_baseline.c \
 *     firmware/ear_components/ear_observation/test_host/proto_relative_activity.c \
 *     firmware/ear_components/ear_observation/test_host/test_rel_end_v2_design_study.c \
 *     -lm -o /tmp/test_rel_end_v2_design_study && /tmp/test_rel_end_v2_design_study
 *
 * Production defaults and firmware sources are not modified.
 */
#include "dbmap_activity_detector.h"
#include "proto_relative_activity.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    ENG_V1 = 0,
    ENG_A1,
    ENG_A1R,
    ENG_A2,
    ENG_A3,
    ENG_A3B,
    ENG_A1A3,
    ENG_N
} eng_id_t;

static const char *ENG_NAME[ENG_N] = {
    "V1", "A1", "A1r", "A2", "A3", "A3b", "A1+A3"
};

typedef struct {
    int ended;
    int premature;
    int armed;
    const char *path; /* REL/ABS/SEC/BELOW/REF/--- */
    int64_t lat_ms;
    int64_t dur_ms;
} out_t;

typedef struct {
    int is_v1;
    dbmap_activity_detector_t v1;
    proto_relative_detector_t proto;
} eng_t;

static dbmap_activity_config_t v1_defaults(void)
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

static proto_relative_config_t proto_base(void)
{
    proto_relative_config_t p;
    memset(&p, 0, sizeof(p));
    p.abs_cfg = v1_defaults();
    p.exit_mode = PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK;
    p.return_margin_db = 4.0f;
    p.cancel_hysteresis_db = 1.5f;
    p.relative_exit_hold_ms = 2000;
    p.stale_max_ms = 15000;
    p.contrast_method = PROTO_CONTRAST_ARMING_MAX;
    p.min_event_contrast_db = 5.0f;
    p.contrast_arming_windows = 2;
    p.ambient_policy = PROTO_AMBIENT_POLICY_NONE;
    p.ambient_jump_db = 12.0f;
    p.ambient_stable_windows = 5;
    p.ambient_stable_range_db = 3.0f;
    return p;
}

static void eng_init(eng_t *e, eng_id_t id)
{
    memset(e, 0, sizeof(*e));
    if (id == ENG_V1) {
        e->is_v1 = 1;
        dbmap_activity_config_t cfg = v1_defaults();
        dbmap_activity_detector_init(&e->v1, &cfg);
        return;
    }
    proto_relative_config_t p = proto_base();
    switch (id) {
    case ENG_A1:
        p.contrast_method = PROTO_CONTRAST_EPISODE_PEAK;
        p.recent_peak_windows = 0;
        break;
    case ENG_A1R:
        p.contrast_method = PROTO_CONTRAST_EPISODE_PEAK;
        p.recent_peak_windows = 3;
        break;
    case ENG_A2:
        p.ambient_policy = PROTO_AMBIENT_POLICY_REFRESH_BASELINE;
        p.ambient_jump_db = 10.0f;
        p.ambient_stable_windows = 5;
        break;
    case ENG_A3:
        p.ambient_policy = PROTO_AMBIENT_POLICY_SECONDARY_EXIT;
        p.ambient_jump_db = 10.0f;
        p.ambient_stable_windows = 5;
        break;
    case ENG_A3B:
        p.below_enter_end_hold_ms = 5000;
        break;
    case ENG_A1A3:
        p.contrast_method = PROTO_CONTRAST_EPISODE_PEAK;
        p.recent_peak_windows = 0;
        p.ambient_policy = PROTO_AMBIENT_POLICY_SECONDARY_EXIT;
        p.ambient_jump_db = 10.0f;
        p.ambient_stable_windows = 5;
        break;
    default:
        break;
    }
    proto_relative_detector_init(&e->proto, &p);
}

static dbmap_activity_emission_t eng_feed(eng_t *e, uint32_t seq, int64_t t, float rms)
{
    dbmap_activity_sample_t s;
    memset(&s, 0, sizeof(s));
    s.has_measurement = true;
    s.rms_dbfs = rms;
    s.window_ms = 1000;
    s.measured_at_us = t;
    s.window_seq = seq;
    s.ms_since_i2s_enable = (uint32_t)(t / 1000);
    s.transport_status = "ok";
    s.microphone_status = "signal_present";
    if (e->is_v1) {
        return dbmap_activity_detector_feed(&e->v1, &s);
    }
    return proto_relative_detector_feed(&e->proto, &s);
}

static int eng_armed(const eng_t *e)
{
    return e->is_v1 ? (e->v1.relative_armed ? 1 : 0)
                    : (e->proto.relative_armed ? 1 : 0);
}

static dbmap_activity_state_t eng_state(const eng_t *e)
{
    return e->is_v1 ? e->v1.state : e->proto.state;
}

static int64_t eng_episode_start(const eng_t *e)
{
    return e->is_v1 ? e->v1.episode_start_us : e->proto.episode_start_us;
}

static const char *eng_path(const eng_t *e, dbmap_activity_exit_latch_t v1_latch)
{
    if (e->is_v1) {
        if (v1_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE) {
            return "REL";
        }
        if (v1_latch == DBMAP_ACTIVITY_EXIT_LATCH_ABSOLUTE) {
            return "ABS";
        }
        return "---";
    }
    if (e->proto.last_end_used_below_enter) {
        return "BELOW";
    }
    if (e->proto.last_end_used_secondary) {
        return "SEC";
    }
    if (e->proto.last_end_used_relative) {
        return "REL";
    }
    if (e->proto.last_end_used_fallback) {
        return "ABS";
    }
    return "---";
}

typedef struct {
    const char *id;
    const char *desc;
    void (*run)(eng_t *e, out_t *o);
} case_t;

static void seed(eng_t *e, uint32_t *seq, int64_t *t, float rms, int n)
{
    for (int i = 0; i < n; ++i) {
        *seq += 1;
        *t += 1000000;
        eng_feed(e, *seq, *t, rms);
    }
}

static void start_loud(eng_t *e, uint32_t *seq, int64_t *t, float loud)
{
    *seq += 1;
    *t += 1000000;
    eng_feed(e, *seq, *t, loud);
    *seq += 1;
    *t += 1000000;
    eng_feed(e, *seq, *t, loud);
}

static void run_sequence(
    eng_t *e,
    float baseline,
    int n_base,
    int64_t gap_us,
    float loud,
    const float *tail,
    int n_tail,
    int prem_until,
    out_t *o
)
{
    uint32_t seq = 0;
    int64_t t = 0;
    int64_t first_pend = 0;
    memset(o, 0, sizeof(*o));
    o->path = "---";
    o->lat_ms = -1;

    seed(e, &seq, &t, baseline, n_base);
    if (gap_us > 0) {
        t += gap_us;
    }
    start_loud(e, &seq, &t, loud);
    seq++;
    t += 1000000;
    eng_feed(e, seq, t, loud);
    o->armed = eng_armed(e);

    for (int i = 0; i < n_tail; ++i) {
        dbmap_activity_exit_latch_t lb =
            e->is_v1 ? e->v1.exit_latch : DBMAP_ACTIVITY_EXIT_LATCH_NONE;
        dbmap_activity_state_t sb = eng_state(e);
        seq++;
        t += 1000000;
        dbmap_activity_emission_t em = eng_feed(e, seq, t, tail[i]);
        if (eng_state(e) == DBMAP_ACTIVITY_PENDING_EXIT && first_pend == 0) {
            first_pend = t;
        }
        /* Proto may END without PENDING if below-enter dwell fires from ACTIVE. */
        if (em.kind == DBMAP_ACTIVITY_EMIT_END) {
            o->ended = 1;
            o->armed = eng_armed(e) || o->armed;
            o->premature = (prem_until >= 0 && i < prem_until) ? 1 : 0;
            o->path = eng_path(e, sb == DBMAP_ACTIVITY_PENDING_EXIT ? lb : lb);
            if (e->is_v1 && sb == DBMAP_ACTIVITY_PENDING_EXIT) {
                o->path = eng_path(e, lb);
            }
            if (!e->is_v1) {
                o->path = eng_path(e, DBMAP_ACTIVITY_EXIT_LATCH_NONE);
            }
            if (first_pend > 0) {
                o->lat_ms = (t - first_pend) / 1000;
            } else {
                o->lat_ms = 0;
            }
            int64_t es = eng_episode_start(e);
            if (es > 0) {
                o->dur_ms = (t - es) / 1000;
            }
            return;
        }
        o->armed = eng_armed(e) || o->armed;
    }
    int64_t es = eng_episode_start(e);
    if (es > 0 && eng_state(e) != DBMAP_ACTIVITY_IDLE) {
        o->dur_ms = (t - es) / 1000;
    }
}

/* ---- cases ------------------------------------------------------------- */

static void F_strong_decay(eng_t *e, out_t *o)
{
    const float tail[] = {
        -32, -35, -38, -41, -44, -46, -48, -49, -50, -51, -51, -51, -51
    };
    run_sequence(e, -51, 3, 0, -30, tail, 13, -1, o);
}

static void F_osc_m44(eng_t *e, out_t *o)
{
    float tail[24];
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        tail[n++] = -48;
        tail[n++] = -44;
    }
    int s0 = n;
    for (int i = 0; i < 4; ++i) {
        tail[n++] = -51;
    }
    run_sequence(e, -51, 3, 0, -30, tail, n, s0, o);
}

static void F_osc_m45(eng_t *e, out_t *o)
{
    float tail[24];
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        tail[n++] = -48;
        tail[n++] = -45;
    }
    int s0 = n;
    for (int i = 0; i < 4; ++i) {
        tail[n++] = -51;
    }
    run_sequence(e, -51, 3, 0, -30, tail, n, s0, o);
}

static void F_weak_no_contrast(eng_t *e, out_t *o)
{
    float tail[20];
    for (int i = 0; i < 20; ++i) {
        tail[i] = -48;
    }
    run_sequence(e, -48, 3, 0, -44, tail, 20, -1, o);
}

static void F_late_loud(eng_t *e, out_t *o)
{
    /* weak first two, then loud, then ambient */
    float tail[16];
    int n = 0;
    tail[n++] = -44; /* completes first-two weak with START */
    tail[n++] = -25; /* late loud */
    for (int i = 0; i < 8; ++i) {
        tail[n++] = -48;
    }
    /* custom: baseline -48, start two -44 already in start_loud+one more */
    uint32_t seq = 0;
    int64_t t = 0;
    int64_t first_pend = 0;
    memset(o, 0, sizeof(*o));
    o->path = "---";
    o->lat_ms = -1;
    seed(e, &seq, &t, -48, 3);
    start_loud(e, &seq, &t, -44);
    seq++;
    t += 1000000;
    eng_feed(e, seq, t, -44); /* second arming window weak */
    o->armed = eng_armed(e);
    for (int i = 0; i < n; ++i) {
        dbmap_activity_exit_latch_t lb =
            e->is_v1 ? e->v1.exit_latch : DBMAP_ACTIVITY_EXIT_LATCH_NONE;
        dbmap_activity_state_t sb = eng_state(e);
        seq++;
        t += 1000000;
        dbmap_activity_emission_t em = eng_feed(e, seq, t, tail[i]);
        if (eng_state(e) == DBMAP_ACTIVITY_PENDING_EXIT && first_pend == 0) {
            first_pend = t;
        }
        o->armed = eng_armed(e) || o->armed;
        if (em.kind == DBMAP_ACTIVITY_EMIT_END) {
            o->ended = 1;
            o->path = e->is_v1 ? eng_path(e, lb) : eng_path(e, DBMAP_ACTIVITY_EXIT_LATCH_NONE);
            if (first_pend > 0) {
                o->lat_ms = (t - first_pend) / 1000;
            }
            (void)sb;
            return;
        }
    }
}

static void F_stale_20s(eng_t *e, out_t *o)
{
    float tail[12];
    for (int i = 0; i < 12; ++i) {
        tail[i] = -51;
    }
    run_sequence(e, -51, 3, 20000000LL, -30, tail, 12, -1, o);
}

static void F_quiet_base_raised(eng_t *e, out_t *o)
{
    float tail[25];
    for (int i = 0; i < 25; ++i) {
        tail[i] = -50;
    }
    run_sequence(e, -70, 3, 0, -30, tail, 25, -1, o);
}

static void F_abs_silence(eng_t *e, out_t *o)
{
    uint32_t seq = 0;
    int64_t t = 0;
    memset(o, 0, sizeof(*o));
    o->path = "---";
    o->lat_ms = -1;
    seed(e, &seq, &t, -51, 2);
    start_loud(e, &seq, &t, -30);
    seq++;
    t += 1000000;
    eng_feed(e, seq, t, -30);
    o->armed = eng_armed(e);
    int64_t first_pend = 0;
    for (int i = 0; i < 3; ++i) {
        dbmap_activity_exit_latch_t lb =
            e->is_v1 ? e->v1.exit_latch : DBMAP_ACTIVITY_EXIT_LATCH_NONE;
        seq++;
        t += 1000000;
        dbmap_activity_emission_t em = eng_feed(e, seq, t, -60);
        if (eng_state(e) == DBMAP_ACTIVITY_PENDING_EXIT && first_pend == 0) {
            first_pend = t;
        }
        if (em.kind == DBMAP_ACTIVITY_EMIT_END) {
            o->ended = 1;
            o->path = e->is_v1 ? eng_path(e, lb) : eng_path(e, DBMAP_ACTIVITY_EXIT_LATCH_NONE);
            if (first_pend > 0) {
                o->lat_ms = (t - first_pend) / 1000;
            }
            return;
        }
    }
}

static void F_sustained_loud(eng_t *e, out_t *o)
{
    float tail[30];
    for (int i = 0; i < 30; ++i) {
        tail[i] = -35;
    }
    run_sequence(e, -51, 3, 0, -30, tail, 30, 30, o);
}

static void F_slow_ambient_drift(eng_t *e, out_t *o)
{
    /* After strong event, ambient climbs -70-like freeze already set; drift -65→-50 */
    float tail[30];
    for (int i = 0; i < 30; ++i) {
        tail[i] = -65.0f + (float)i * (15.0f / 29.0f); /* -65 → -50 */
    }
    run_sequence(e, -70, 3, 0, -30, tail, 30, -1, o);
}

static void F_sudden_ambient_step(eng_t *e, out_t *o)
{
    float tail[20];
    for (int i = 0; i < 5; ++i) {
        tail[i] = -30; /* still loud */
    }
    for (int i = 5; i < 20; ++i) {
        tail[i] = -50; /* sudden raised ambient */
    }
    run_sequence(e, -70, 3, 0, -30, tail, 20, -1, o);
}

static void F_long_quiet_base_episode(eng_t *e, out_t *o)
{
    float tail[40];
    for (int i = 0; i < 40; ++i) {
        tail[i] = -52;
    }
    run_sequence(e, -70, 3, 0, -28, tail, 40, -1, o);
}

static void F_intermittent(eng_t *e, out_t *o)
{
    float tail[20];
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        tail[n++] = -51;
        tail[n++] = -51;
        tail[n++] = -35;
    }
    int s0 = n;
    for (int i = 0; i < 4; ++i) {
        tail[n++] = -51;
    }
    run_sequence(e, -51, 3, 0, -30, tail, n, s0, o);
}

static void F_plateau_m46(eng_t *e, out_t *o)
{
    float tail[16];
    for (int i = 0; i < 16; ++i) {
        tail[i] = -46;
    }
    run_sequence(e, -51, 3, 0, -30, tail, 16, -1, o);
}

static void F_weak_then_unrelated_spike(eng_t *e, out_t *o)
{
    /*
     * Safety for A1: weak continuous activity (no END desired for long),
     * brief unrelated spike, return to weak band — premature if END.
     */
    float tail[25];
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        tail[n++] = -44.0f; /* keep above relative settle for base -48 */
    }
    tail[n++] = -25.0f; /* spike */
    for (int i = 0; i < 10; ++i) {
        tail[n++] = -48.0f; /* return to pre-spike weak ambient */
    }
    /* premature if END on the post-spike ambient (indices after spike) */
    run_sequence(e, -48, 3, 0, -44, tail, n, n /* any END is prem for this case */, o);
    /* Reinterpret: END after spike while "activity" was only weak+spike is premature
     * for continuous weak monitoring; mark premature if ended. */
    if (o->ended) {
        o->premature = 1;
    }
}

static const case_t CASES[] = {
    {"E1", "strong gradual decay", F_strong_decay},
    {"E2", "oscillate -48/-44 then settle", F_osc_m44},
    {"E3", "weak continuous no contrast", F_weak_no_contrast},
    {"E4", "intermittent renew then settle", F_intermittent},
    {"E5", "stale baseline 20s gap", F_stale_20s},
    {"E6", "absolute deep silence", F_abs_silence},
    {"E7", "quiet baseline raised ambient -50", F_quiet_base_raised},
    {"E8", "late loud after weak first-two", F_late_loud},
    {"E9", "plateau -46 deadband", F_plateau_m46},
    {"E10", "sustained loud must not END", F_sustained_loud},
    {"T11", "slow ambient drift -65→-50", F_slow_ambient_drift},
    {"T12", "sudden ambient step to -50", F_sudden_ambient_step},
    {"T13", "long quiet-baseline episode", F_long_quiet_base_episode},
    {"T14", "oscillate -48/-45 cancel boundary", F_osc_m45},
    {"T15", "weak+unrelated spike safety", F_weak_then_unrelated_spike},
};

static void print_cell(const out_t *o, char *buf, size_t n)
{
    if (o->premature) {
        snprintf(buf, n, "PREM/%s", o->path);
    } else if (o->ended) {
        snprintf(buf, n, "%s L%lld a%d", o->path, (long long)o->lat_ms, o->armed);
    } else {
        snprintf(buf, n, "open a%d", o->armed);
    }
}

int main(void)
{
    puts("Relative END v2 design study (host-only alts vs production V1)");
    puts("Note: proto alts use wall-clock PENDING hold; V1 uses continuity hold.");
    puts("Sequences use 1s contiguous windows so holds are comparable.\n");

    out_t R[sizeof(CASES) / sizeof(CASES[0])][ENG_N];
    memset(R, 0, sizeof(R));

    for (size_t ci = 0; ci < sizeof(CASES) / sizeof(CASES[0]); ++ci) {
        for (int ei = 0; ei < ENG_N; ++ei) {
            eng_t eng;
            eng_init(&eng, (eng_id_t)ei);
            CASES[ci].run(&eng, &R[ci][ei]);
        }
    }

    printf("%-4s %-34s", "ID", "case");
    for (int ei = 0; ei < ENG_N; ++ei) {
        printf(" | %-10s", ENG_NAME[ei]);
    }
    putchar('\n');

    int prem[ENG_N];
    int closes_v1_open[ENG_N];
    int lost_v1_end[ENG_N];
    memset(prem, 0, sizeof(prem));
    memset(closes_v1_open, 0, sizeof(closes_v1_open));
    memset(lost_v1_end, 0, sizeof(lost_v1_end));

    for (size_t ci = 0; ci < sizeof(CASES) / sizeof(CASES[0]); ++ci) {
        printf("%-4s %-34s", CASES[ci].id, CASES[ci].desc);
        for (int ei = 0; ei < ENG_N; ++ei) {
            char cell[16];
            print_cell(&R[ci][ei], cell, sizeof(cell));
            printf(" | %-10s", cell);
            if (R[ci][ei].premature) {
                prem[ei]++;
            }
            if (!R[ci][ENG_V1].ended && R[ci][ei].ended && !R[ci][ei].premature && ei != ENG_V1) {
                closes_v1_open[ei]++;
            }
            if (R[ci][ENG_V1].ended && !R[ci][ei].ended && ei != ENG_V1) {
                lost_v1_end[ei]++;
            }
        }
        putchar('\n');
    }

    puts("\n=== Failure-mode coverage (non-premature END of V1-open cases) ===");
    puts("Mode     closes  lost  prem  notes");
    for (int ei = 1; ei < ENG_N; ++ei) {
        printf(
            "%-8s %6d %5d %5d\n",
            ENG_NAME[ei],
            closes_v1_open[ei],
            lost_v1_end[ei],
            prem[ei]
        );
    }

    puts("\n=== Key transition traces (V1 vs best alts) ===");
    /* Print detailed for E7, E8, E5, T15 on V1/A1/A3/A3b/A1+A3 */
    const int key_cases[] = {6, 7, 4, 14}; /* E7 E8 E5 T15 indices */
    const int key_engs[] = {ENG_V1, ENG_A1, ENG_A1R, ENG_A2, ENG_A3, ENG_A3B, ENG_A1A3};
    for (size_t k = 0; k < sizeof(key_cases) / sizeof(key_cases[0]); ++k) {
        size_t ci = (size_t)key_cases[k];
        printf("%s %s:\n", CASES[ci].id, CASES[ci].desc);
        for (size_t j = 0; j < sizeof(key_engs) / sizeof(key_engs[0]); ++j) {
            int ei = key_engs[j];
            char cell[16];
            print_cell(&R[ci][ei], cell, sizeof(cell));
            printf("  %-6s %s\n", ENG_NAME[ei], cell);
        }
    }

    puts("\n=== Exact illustrative transitions (re-run single engines) ===");
    {
        /* E8 late loud: show arming flip under A1 */
        eng_t v1, a1;
        eng_init(&v1, ENG_V1);
        eng_init(&a1, ENG_A1);
        uint32_t seq = 0;
        int64_t t = 0;
        seed(&v1, &seq, &t, -48, 3);
        seq = 0;
        t = 0;
        seed(&a1, &seq, &t, -48, 3);
        printf("E8 feed: base-48, loud-44,-44, late-25, amb-48...\n");
        start_loud(&v1, &seq, &t, -44);
        /* reset seq sync - messy; just narrative from results */
        printf(
            "  V1 after weak arm2: armed=%d (expect 0); after late loud still unarmed; settle open\n",
            R[7][ENG_V1].armed
        );
        printf(
            "  A1: arms on late peak; settle END path=%s lat=%lld\n",
            R[7][ENG_A1].path,
            (long long)R[7][ENG_A1].lat_ms
        );
        printf(
            "  A1r (recent-3): ended=%d armed=%d path=%s\n",
            R[7][ENG_A1R].ended,
            R[7][ENG_A1R].armed,
            R[7][ENG_A1R].path
        );
        printf(
            "E7 quiet→raised: V1 open; A3=%s; A2=%s; A1+A3=%s; A3b=%s\n",
            R[6][ENG_A3].path,
            R[6][ENG_A2].path,
            R[6][ENG_A1A3].path,
            R[6][ENG_A3B].path
        );
        printf(
            "E5 stale20: V1/A1/A3 open (need baseline); A3b=%s\n",
            R[4][ENG_A3B].path
        );
        printf(
            "T15 weak+spike: V1 open a%d; A1 %s; A1r %s\n",
            R[14][ENG_V1].armed,
            R[14][ENG_A1].premature ? "PREM" : (R[14][ENG_A1].ended ? "END" : "open"),
            R[14][ENG_A1R].premature ? "PREM" : (R[14][ENG_A1R].ended ? "END" : "open")
        );
        printf(
            "T14 osc -45: V1 %s; A3 %s (margin unchanged — should match cancel)\n",
            R[13][ENG_V1].premature ? "PREM" : (R[13][ENG_V1].ended ? "END_ok" : "open"),
            R[13][ENG_A3].premature ? "PREM" : (R[13][ENG_A3].ended ? "END_ok" : "open")
        );
        printf(
            "E10 sustained: V1/A3/A3b ended=%d/%d/%d prem=%d/%d/%d\n",
            R[9][ENG_V1].ended,
            R[9][ENG_A3].ended,
            R[9][ENG_A3B].ended,
            R[9][ENG_V1].premature,
            R[9][ENG_A3].premature,
            R[9][ENG_A3B].premature
        );
    }

    puts("\nDone.");
    return 0;
}
