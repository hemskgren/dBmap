/**
 * Host robustness evaluation for relative END safeguards.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     -I firmware/ear_components/ear_observation/test_host \
 *     firmware/ear_components/ear_observation/activity_detector.c \
 *     firmware/ear_components/ear_observation/test_host/proto_pre_event_baseline.c \
 *     firmware/ear_components/ear_observation/test_host/proto_relative_activity.c \
 *     firmware/ear_components/ear_observation/test_host/test_proto_baseline_robustness.c \
 *     -lm -o /tmp/test_proto_baseline_robustness && /tmp/test_proto_baseline_robustness
 */
#include "dbmap_activity_detector.h"
#include "proto_pre_event_baseline.h"
#include "proto_relative_activity.h"

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

typedef struct {
    uint32_t seq;
    float rms;
    bool usable;
} rms_step_t;

typedef struct {
    int64_t start_us;
    int64_t end_us;
    bool got_start;
    bool got_end;
    bool used_relative;
    bool used_fallback;
    bool used_secondary;
    bool relative_armed;
    bool contrast_blocked;
    bool ambient_shift;
    bool reused_prior;
    float baseline_dbfs;
    uint32_t baseline_n;
    bool baseline_reliable;
    float measured_contrast_db;
} run_result_t;

static dbmap_activity_config_t abs_cfg_default(void)
{
    dbmap_activity_config_t cfg = {
        .enter_threshold_dbfs = -45.0f,
        .exit_threshold_dbfs = -55.0f,
        .min_activity_ms = 2000,
        .exit_hold_ms = 1000,
        .boot_settle_ms = 0,
        .observation_interval_ms = 5000,
        .max_invalid_hold_ms = 30000,
    };
    return cfg;
}

static void cfg_base(proto_relative_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->abs_cfg = abs_cfg_default();
    cfg->exit_mode = PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK;
    cfg->return_margin_db = 4.0f;
    cfg->cancel_hysteresis_db = 1.0f;
    cfg->relative_exit_hold_ms = 2000;
    cfg->stale_max_ms = 15000;
    cfg->contrast_method = PROTO_CONTRAST_DISABLED;
    cfg->min_event_contrast_db = 0.0f;
    cfg->contrast_arming_windows = 2;
    cfg->ambient_policy = PROTO_AMBIENT_POLICY_NONE;
    cfg->ambient_jump_db = 15.0f;
    cfg->ambient_stable_windows = 3;
    cfg->ambient_stable_range_db = 3.0f;
    cfg->reuse_prior_baseline_max_age_ms = 0;
}

static dbmap_activity_sample_t make_sample(uint32_t seq, int64_t t_us, float rms, bool usable)
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
    s.microphone_status = usable ? "signal_present" : "disconnected_suspected";
    return s;
}

static run_result_t run_steps(const rms_step_t *steps, size_t n, const proto_relative_config_t *cfg)
{
    proto_relative_detector_t det;
    proto_relative_detector_init(&det, cfg);
    run_result_t r;
    memset(&r, 0, sizeof(r));

    for (size_t i = 0; i < n; ++i) {
        const int64_t t = (int64_t)steps[i].seq * 1000000LL;
        dbmap_activity_sample_t s =
            make_sample(steps[i].seq, t, steps[i].rms, steps[i].usable);
        dbmap_activity_emission_t e = proto_relative_detector_feed(&det, &s);
        if (e.kind == DBMAP_ACTIVITY_EMIT_START) {
            r.got_start = true;
            r.got_end = false;
            r.start_us = e.interval_end_mono_us;
            r.end_us = 0;
            r.used_relative = false;
            r.used_fallback = false;
            r.used_secondary = false;
            r.baseline_reliable = det.baseline.reliable;
            r.baseline_dbfs = det.baseline.frozen_baseline_dbfs;
            r.baseline_n = det.baseline.frozen_sample_count;
            r.relative_armed = det.relative_armed;
            r.measured_contrast_db = det.measured_contrast_db;
            r.reused_prior = det.reused_prior_baseline;
            r.contrast_blocked = false;
            r.ambient_shift = false;
        } else if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            r.relative_armed = det.last_end_relative_armed;
            r.measured_contrast_db = det.last_end_contrast_db;
            r.contrast_blocked = det.contrast_blocked_end;
            r.ambient_shift = det.ambient_shift_suspected;
            r.got_end = true;
            r.end_us = e.interval_end_mono_us;
            r.used_relative = det.last_end_used_relative;
            r.used_fallback = det.last_end_used_fallback;
            r.used_secondary = det.last_end_used_secondary;
        } else {
            if (det.relative_armed) {
                r.relative_armed = true;
            }
            r.measured_contrast_db = det.measured_contrast_db;
            if (det.contrast_blocked_end) {
                r.contrast_blocked = true;
            }
            if (det.ambient_shift_suspected) {
                r.ambient_shift = true;
            }
        }
    }
    return r;
}

static void print_r(const char *label, const run_result_t *r)
{
    printf(
        "  %-36s start=%s@%lld end=%s@%lld lat=%lld base=%s n=%u med=%.1f "
        "armed=%d ctr=%.1f blk=%d amb=%d sec=%d rel=%d fb=%d reuse=%d\n",
        label,
        r->got_start ? "Y" : "N",
        (long long)(r->got_start ? r->start_us / 1000 : -1),
        r->got_end ? "Y" : "N",
        (long long)(r->got_end ? r->end_us / 1000 : -1),
        (long long)(r->got_start && r->got_end ? (r->end_us - r->start_us) / 1000 : -1),
        r->baseline_reliable ? "Y" : "N",
        r->baseline_n,
        r->baseline_reliable ? (double)r->baseline_dbfs : 0.0,
        r->relative_armed ? 1 : 0,
        (double)r->measured_contrast_db,
        r->contrast_blocked ? 1 : 0,
        r->ambient_shift ? 1 : 0,
        r->used_secondary ? 1 : 0,
        r->used_relative ? 1 : 0,
        r->used_fallback ? 1 : 0,
        r->reused_prior ? 1 : 0
    );
}

static void fill_quiet(rms_step_t *out, uint32_t start_seq, uint32_t n, float rms)
{
    for (uint32_t i = 0; i < n; ++i) {
        out[i].seq = start_seq + i;
        out[i].rms = rms;
        out[i].usable = true;
    }
}

/* ---- Investigation A: contrast ---- */

static void build_s11(rms_step_t *steps, size_t *n_out)
{
    fill_quiet(steps, 1, 5, -49.0f);
    steps[5] = (rms_step_t){6, -44.0f, true};
    steps[6] = (rms_step_t){7, -44.0f, true};
    steps[7] = (rms_step_t){8, -44.5f, true};
    steps[8] = (rms_step_t){9, -44.5f, true};
    steps[9] = (rms_step_t){10, -44.5f, true};
    for (int i = 10; i < 14; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -49.0f, true};
    }
    *n_out = 14;
}

static void test_contrast_s11(void)
{
    printf("\n== A: S11 weak event, margin 6, contrast variants (synthetic) ==\n");
    rms_step_t steps[16];
    size_t n;
    build_s11(steps, &n);

    proto_relative_config_t cfg;
    cfg_base(&cfg);
    cfg.return_margin_db = 6.0f;
    cfg.relative_exit_hold_ms = 1000;
    cfg.cancel_hysteresis_db = 1.0f;

    run_result_t off = run_steps(steps, n, &cfg);
    print_r("contrast OFF (ref early END)", &off);
    EXPECT(off.got_end && off.end_us <= 9 * 1000000LL);

    cfg.contrast_method = PROTO_CONTRAST_START_WINDOW;
    cfg.min_event_contrast_db = 3.0f;
    run_result_t c3 = run_steps(steps, n, &cfg);
    print_r("START_WINDOW min=3", &c3);

    cfg.min_event_contrast_db = 5.0f;
    run_result_t c5 = run_steps(steps, n, &cfg);
    print_r("START_WINDOW min=5", &c5);

    cfg.min_event_contrast_db = 8.0f;
    run_result_t c8 = run_steps(steps, n, &cfg);
    print_r("START_WINDOW min=8", &c8);

    /*
     * START contrast = -44 - (-49) = 5 dB.
     * Relative exit thr with margin 6 = -43. Weak -44.5 already satisfies END.
     * min_contrast 3 or 5 still arms → premature END. min 8 blocks arming.
     */
    EXPECT(fabsf(c3.measured_contrast_db - 5.0f) < 0.2f);
    EXPECT(c3.relative_armed);
    EXPECT(c3.got_end && c3.end_us <= 9 * 1000000LL);
    EXPECT(c5.relative_armed);
    EXPECT(c5.got_end && c5.end_us <= 9 * 1000000LL);
    EXPECT(!c8.relative_armed);
    EXPECT(c8.contrast_blocked);
    EXPECT(!c8.got_end);

    /* Coupled rule candidate: min_contrast = return_margin + 2 (=8). */
    cfg.min_event_contrast_db = cfg.return_margin_db + 2.0f;
    run_result_t coupled = run_steps(steps, n, &cfg);
    print_r("START_WINDOW min=margin+2", &coupled);
    EXPECT(!coupled.relative_armed);
    EXPECT(!coupled.got_end);

    cfg.contrast_method = PROTO_CONTRAST_ARMING_MAX;
    cfg.min_event_contrast_db = 5.0f;
    cfg.contrast_arming_windows = 3;
    run_result_t amax = run_steps(steps, n, &cfg);
    print_r("ARMING_MAX w=3 min=5 (still early)", &amax);
    EXPECT(amax.relative_armed);
    EXPECT(amax.got_end && amax.end_us <= 9 * 1000000LL);

    cfg.contrast_method = PROTO_CONTRAST_ARMING_MEDIAN;
    run_result_t amed = run_steps(steps, n, &cfg);
    print_r("ARMING_MEDIAN w=3 min=5", &amed);
}

static void test_contrast_strong_event_still_ends(void)
{
    printf("\n== A: strong event still ENDs with contrast=5 (synthetic) ==\n");
    rms_step_t steps[18];
    fill_quiet(steps, 1, 5, -52.0f);
    steps[5] = (rms_step_t){6, -35.0f, true};
    steps[6] = (rms_step_t){7, -34.0f, true};
    for (int i = 7; i < 11; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -35.0f, true};
    }
    for (int i = 11; i < 17; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -52.0f, true};
    }

    proto_relative_config_t cfg;
    cfg_base(&cfg);
    cfg.contrast_method = PROTO_CONTRAST_START_WINDOW;
    cfg.min_event_contrast_db = 5.0f;
    cfg.return_margin_db = 4.0f;
    cfg.relative_exit_hold_ms = 2000;

    run_result_t r = run_steps(steps, 17, &cfg);
    print_r("strong + contrast5", &r);
    EXPECT(r.relative_armed);
    EXPECT(r.got_end && r.used_relative);
    EXPECT(r.measured_contrast_db >= 5.0f);
}

/* ---- Investigation B: ambient jump ---- */

static void build_s6(rms_step_t *steps, size_t *n_out)
{
    fill_quiet(steps, 1, 4, -70.0f);
    steps[4] = (rms_step_t){5, -40.0f, true};
    steps[5] = (rms_step_t){6, -40.0f, true};
    for (int i = 6; i < 12; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -40.0f, true};
    }
    for (int i = 12; i < 22; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -48.0f, true};
    }
    *n_out = 22;
}

static void test_ambient_policies(void)
{
    printf("\n== B: S6 ambient step −70→−48 (synthetic) ==\n");
    rms_step_t steps[24];
    size_t n;
    build_s6(steps, &n);

    proto_relative_config_t cfg;
    cfg_base(&cfg);
    cfg.return_margin_db = 4.0f;
    cfg.relative_exit_hold_ms = 1000;

    cfg.ambient_policy = PROTO_AMBIENT_POLICY_NONE;
    run_result_t none = run_steps(steps, n, &cfg);
    print_r("policy NONE (stuck)", &none);
    EXPECT(none.got_start && !none.got_end);

    cfg.ambient_policy = PROTO_AMBIENT_POLICY_INDICATOR;
    cfg.ambient_jump_db = 15.0f;
    cfg.ambient_stable_windows = 3;
    run_result_t ind = run_steps(steps, n, &cfg);
    print_r("policy INDICATOR", &ind);
    EXPECT(ind.ambient_shift);
    EXPECT(!ind.got_end); /* indicator alone does not END */

    cfg.ambient_policy = PROTO_AMBIENT_POLICY_SECONDARY_EXIT;
    run_result_t sec = run_steps(steps, n, &cfg);
    print_r("policy SECONDARY_EXIT", &sec);
    EXPECT(sec.ambient_shift);
    EXPECT(sec.got_end && sec.used_secondary);
}

static void test_gradual_ambient_vs_fade(void)
{
    printf("\n== B: gradual ambient rise vs gradual source fade (synthetic) ==\n");
    /* Gradual ambient: after event, climb -70 → -48 over many seconds below enter */
    rms_step_t amb[30];
    fill_quiet(amb, 1, 4, -70.0f);
    amb[4] = (rms_step_t){5, -40.0f, true};
    amb[5] = (rms_step_t){6, -40.0f, true};
    amb[6] = (rms_step_t){7, -40.0f, true};
    const float climb[] = {-60.f, -56.f, -52.f, -50.f, -49.f, -48.f, -48.f, -48.f, -48.f};
    for (int i = 0; i < 9; ++i) {
        amb[7 + i] = (rms_step_t){(uint32_t)(8 + i), climb[i], true};
    }

    proto_relative_config_t cfg;
    cfg_base(&cfg);
    cfg.ambient_policy = PROTO_AMBIENT_POLICY_SECONDARY_EXIT;
    cfg.ambient_jump_db = 15.0f;
    cfg.ambient_stable_windows = 3;
    cfg.relative_exit_hold_ms = 1000;

    run_result_t g = run_steps(amb, 16, &cfg);
    print_r("gradual ambient + secondary", &g);

    /* Strong source fade back to original baseline — primary relative should END */
    rms_step_t fade[20];
    fill_quiet(fade, 1, 4, -62.0f);
    fade[4] = (rms_step_t){5, -36.0f, true};
    fade[5] = (rms_step_t){6, -36.0f, true};
    const float fseq[] = {-38.f, -42.f, -46.f, -50.f, -54.f, -58.f, -62.f, -62.f, -62.f};
    for (int i = 0; i < 9; ++i) {
        fade[6 + i] = (rms_step_t){(uint32_t)(7 + i), fseq[i], true};
    }
    cfg.ambient_policy = PROTO_AMBIENT_POLICY_SECONDARY_EXIT;
    run_result_t f = run_steps(fade, 15, &cfg);
    print_r("source fade to baseline", &f);
    EXPECT(f.got_end && f.used_relative);
    EXPECT(!f.used_secondary); /* primary path */
}

static void test_invalid_during_ambient(void)
{
    printf("\n== B: invalid windows during ambient step (synthetic) ==\n");
    rms_step_t steps[24];
    size_t n;
    build_s6(steps, &n);
    steps[13] = (rms_step_t){14, -120.0f, false};
    steps[14] = (rms_step_t){15, -120.0f, false};

    proto_relative_config_t cfg;
    cfg_base(&cfg);
    cfg.ambient_policy = PROTO_AMBIENT_POLICY_SECONDARY_EXIT;
    cfg.ambient_jump_db = 15.0f;
    cfg.ambient_stable_windows = 3;
    cfg.relative_exit_hold_ms = 1000;

    run_result_t r = run_steps(steps, n, &cfg);
    print_r("secondary + invalid gap", &r);
    EXPECT(r.got_end); /* must still END after usable stability resumes */
}

/* ---- Investigation C: back-to-back ---- */

/**
 * Ambient stays at -50 (absolute END never fires).
 * quiet_gap_s = usable quiet windows between loud sections.
 * With hold=1s, the first ~2 quiet windows close episode 1; remaining
 * windows refill the pre-event ring before episode 2.
 */
static size_t build_two_events(rms_step_t *steps, uint32_t quiet_gap_s)
{
    const float amb = -50.0f;
    uint32_t seq = 1;
    size_t i = 0;
    for (uint32_t k = 0; k < 4; ++k) {
        steps[i++] = (rms_step_t){seq++, amb, true};
    }
    steps[i++] = (rms_step_t){seq++, -35.0f, true};
    steps[i++] = (rms_step_t){seq++, -35.0f, true};
    steps[i++] = (rms_step_t){seq++, -35.0f, true};
    for (uint32_t k = 0; k < quiet_gap_s; ++k) {
        steps[i++] = (rms_step_t){seq++, amb, true};
    }
    steps[i++] = (rms_step_t){seq++, -35.0f, true};
    steps[i++] = (rms_step_t){seq++, -35.0f, true};
    for (uint32_t k = 0; k < 6; ++k) {
        steps[i++] = (rms_step_t){seq++, amb, true};
    }
    return i;
}

static void test_back_to_back_gaps(void)
{
    printf("\n== C: back-to-back gaps 1/2/3/5 s @ ambient -50 (synthetic) ==\n");
    printf("  Note: hold=1s consumes ~2 quiet windows to END episode 1.\n");
    const uint32_t gaps[] = {1, 2, 3, 5};
    for (size_t gi = 0; gi < 4; ++gi) {
        rms_step_t steps[40];
        size_t n = build_two_events(steps, gaps[gi]);

        proto_relative_config_t cfg;
        cfg_base(&cfg);
        cfg.return_margin_db = 4.0f;
        cfg.relative_exit_hold_ms = 1000;
        cfg.reuse_prior_baseline_max_age_ms = 0;

        char label[64];
        snprintf(label, sizeof(label), "gap=%us no-reuse", gaps[gi]);
        run_result_t r = run_steps(steps, n, &cfg);
        print_r(label, &r);

        cfg.reuse_prior_baseline_max_age_ms = 10000;
        snprintf(label, sizeof(label), "gap=%us reuse≤10s", gaps[gi]);
        run_result_t rr = run_steps(steps, n, &cfg);
        print_r(label, &rr);

        if (gaps[gi] <= 1) {
            /* Too short to natural-END before next loud → merged episode; still ENDs at final -50. */
            EXPECT(r.got_end);
        } else if (gaps[gi] == 2) {
            /* END on 2nd quiet then immediate 2nd START with empty ring. */
            EXPECT(!r.baseline_reliable);
            EXPECT(!r.got_end); /* absolute fallback cannot END at -50 */
            EXPECT(rr.reused_prior);
            EXPECT(rr.got_end);
        } else if (gaps[gi] == 3) {
            /* 2 quiet to END + 1 idle quiet → still <3 samples. */
            EXPECT(!r.baseline_reliable);
            EXPECT(!r.got_end);
            EXPECT(rr.reused_prior && rr.got_end);
        } else {
            /* gap 5: 2 to END + 3 idle → reliable fresh baseline. */
            EXPECT(r.baseline_reliable);
            EXPECT(r.got_end);
            EXPECT(rr.got_end);
        }
    }
}

static void test_pause_then_resume(void)
{
    printf("\n== pause then resume with contrast+hold (synthetic) ==\n");
    rms_step_t steps[22];
    fill_quiet(steps, 1, 4, -65.0f);
    steps[4] = (rms_step_t){5, -38.0f, true};
    steps[5] = (rms_step_t){6, -38.0f, true};
    for (int i = 6; i <= 10; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -38.0f, true};
    }
    steps[11] = (rms_step_t){12, -65.0f, true};
    for (int i = 12; i <= 16; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -38.0f, true};
    }
    steps[17] = (rms_step_t){18, -65.0f, true};
    steps[18] = (rms_step_t){19, -65.0f, true};
    steps[19] = (rms_step_t){20, -65.0f, true};

    proto_relative_config_t cfg;
    cfg_base(&cfg);
    cfg.contrast_method = PROTO_CONTRAST_START_WINDOW;
    cfg.min_event_contrast_db = 5.0f;
    cfg.relative_exit_hold_ms = 2000;
    cfg.return_margin_db = 4.0f;

    run_result_t r = run_steps(steps, 20, &cfg);
    print_r("pause1s hold2 contrast5", &r);
    EXPECT(r.got_end);
    EXPECT(r.end_us >= 19 * 1000000LL);
}

/* ---- Combined meaningful matrix ---- */

static void test_param_matrix(void)
{
    printf("\n== Combined matrix (selected cells, synthetic) ==\n");
    printf("  Focus: ambient-50 return (S2-like) + weak S11 + S6 secondary\n");

    rms_step_t s2[20];
    fill_quiet(s2, 1, 5, -50.0f);
    s2[5] = (rms_step_t){6, -35.0f, true};
    s2[6] = (rms_step_t){7, -35.0f, true};
    for (int i = 7; i <= 10; ++i) {
        s2[i] = (rms_step_t){(uint32_t)(i + 1), -35.0f, true};
    }
    for (int i = 11; i < 18; ++i) {
        s2[i] = (rms_step_t){(uint32_t)(i + 1), -50.0f, true};
    }

    const float margins[] = {2.f, 4.f, 6.f};
    const uint32_t holds[] = {1000, 2000, 3000};
    const float hysts[] = {1.f, 1.5f, 2.f};

    printf("  S2-like ambient-50 | margin | hold | hyst | end | lat_ms | armed\n");
    for (size_t mi = 0; mi < 3; ++mi) {
        for (size_t hi = 0; hi < 3; ++hi) {
            /* Keep matrix small: only hyst 1.5 for middle band except margin 4 */
            for (size_t yi = 0; yi < 3; ++yi) {
                if (margins[mi] != 4.0f && hysts[yi] != 1.5f) {
                    continue;
                }
                if (margins[mi] == 4.0f && holds[hi] != 2000 && hysts[yi] != 1.5f) {
                    continue;
                }
                proto_relative_config_t cfg;
                cfg_base(&cfg);
                cfg.return_margin_db = margins[mi];
                cfg.relative_exit_hold_ms = holds[hi];
                cfg.cancel_hysteresis_db = hysts[yi];
                cfg.contrast_method = PROTO_CONTRAST_START_WINDOW;
                cfg.min_event_contrast_db = 5.0f;
                run_result_t r = run_steps(s2, 18, &cfg);
                printf(
                    "    m=%.0f h=%u hy=%.1f → end=%d lat=%lld armed=%d\n",
                    (double)margins[mi],
                    holds[hi],
                    (double)hysts[yi],
                    r.got_end ? 1 : 0,
                    (long long)(r.got_start && r.got_end ? (r.end_us - r.start_us) / 1000 : -1),
                    r.relative_armed ? 1 : 0
                );
                EXPECT(r.got_end);
            }
        }
    }
}

static void print_replay_format(void)
{
    printf("\n== Future EAR-001 RMS replay format (not available now) ==\n");
    printf("  CSV, one row per analysis window:\n");
    printf("    window_seq,measured_at_mono_us,ms_since_i2s_enable,rms_dbfs,peak_dbfs,");
    printf("transport_status,microphone_status,usable\n");
    printf("  Example:\n");
    printf("    12,12000000,12000,-51.68,-41.69,ok,signal_present,1\n");
    printf("  No raw device sequences in-repo; all tests above are synthetic.\n");
}

static void print_resources(void)
{
    printf("\n== Resource delta vs Step B prototype ==\n");
    printf("  proto_relative_detector_t ≈ %zu bytes (+contrast/ambient/prior fields)\n",
           sizeof(proto_relative_detector_t));
    printf("  Extra CPU/window: contrast arm O(1–8); ambient stable O(1–8); no PCM/heap\n");
    printf("  Stack: small temps for median ≤8 floats\n");
}

int main(void)
{
    printf("proto baseline END robustness evaluation (host-only)\n");

    test_contrast_s11();
    test_contrast_strong_event_still_ends();
    test_ambient_policies();
    test_gradual_ambient_vs_fade();
    test_invalid_during_ambient();
    test_back_to_back_gaps();
    test_pause_then_resume();
    test_param_matrix();
    print_replay_format();
    print_resources();

    if (g_failures) {
        fprintf(stderr, "\n%d failure(s)\n", g_failures);
        return 1;
    }
    printf("\nAll robustness expectations passed.\n");
    return 0;
}
