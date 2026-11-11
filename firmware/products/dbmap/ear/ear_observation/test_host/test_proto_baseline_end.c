/**
 * Host evaluation: frozen pre-event RMS baseline vs absolute END.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     -I firmware/ear_components/ear_observation/test_host \
 *     firmware/ear_components/ear_observation/activity_detector.c \
 *     firmware/ear_components/ear_observation/test_host/proto_pre_event_baseline.c \
 *     firmware/ear_components/ear_observation/test_host/proto_relative_activity.c \
 *     firmware/ear_components/ear_observation/test_host/test_proto_baseline_end.c \
 *     -lm -o /tmp/test_proto_baseline_end && /tmp/test_proto_baseline_end
 *
 * Production firmware is not linked into the device image from these files.
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
    bool usable; /* false → hard mic fault (invalid, not quiet) */
} rms_step_t;

typedef struct {
    int64_t start_us;
    int64_t end_us;
    bool got_start;
    bool got_end;
    bool used_relative;
    bool used_fallback;
    float baseline_dbfs;
    uint32_t baseline_n;
    bool baseline_reliable;
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

static dbmap_activity_sample_t make_sample(
    uint32_t seq,
    int64_t t_us,
    float rms,
    bool usable
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
    if (usable) {
        s.transport_status = "ok";
        s.microphone_status = "signal_present";
    } else {
        s.transport_status = "ok";
        s.microphone_status = "disconnected_suspected";
    }
    return s;
}

static run_result_t run_proto(
    const rms_step_t *steps,
    size_t n,
    proto_exit_mode_t mode,
    float margin_db,
    uint32_t hold_ms,
    float cancel_hyst_db
)
{
    proto_relative_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.abs_cfg = abs_cfg_default();
    cfg.exit_mode = mode;
    cfg.return_margin_db = margin_db;
    cfg.cancel_hysteresis_db = cancel_hyst_db;
    cfg.relative_exit_hold_ms = hold_ms;
    cfg.stale_max_ms = 15000;

    proto_relative_detector_t det;
    proto_relative_detector_init(&det, &cfg);

    run_result_t r;
    memset(&r, 0, sizeof(r));

    for (size_t i = 0; i < n; ++i) {
        const int64_t t = (int64_t)steps[i].seq * 1000000LL;
        dbmap_activity_sample_t s =
            make_sample(steps[i].seq, t, steps[i].rms, steps[i].usable);
        dbmap_activity_emission_t e = proto_relative_detector_feed(&det, &s);
        if (e.kind == DBMAP_ACTIVITY_EMIT_START) {
            /* Track the latest episode only. */
            r.got_start = true;
            r.got_end = false;
            r.start_us = e.interval_end_mono_us;
            r.end_us = 0;
            r.used_relative = false;
            r.used_fallback = false;
            r.baseline_reliable = det.baseline.reliable;
            r.baseline_dbfs = det.baseline.frozen_baseline_dbfs;
            r.baseline_n = det.baseline.frozen_sample_count;
        } else if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            r.got_end = true;
            r.end_us = e.interval_end_mono_us;
            r.used_relative = det.last_end_used_relative;
            r.used_fallback = det.last_end_used_fallback;
        }
    }
    return r;
}

static void print_result(const char *label, const run_result_t *r)
{
    printf(
        "  %-28s start=%s@%lld end=%s@%lld base=%s n=%u med=%.2f rel=%d fb=%d lat_ms=%lld\n",
        label,
        r->got_start ? "Y" : "N",
        (long long)(r->got_start ? r->start_us / 1000 : -1),
        r->got_end ? "Y" : "N",
        (long long)(r->got_end ? r->end_us / 1000 : -1),
        r->baseline_reliable ? "Y" : "N",
        r->baseline_n,
        r->baseline_reliable ? (double)r->baseline_dbfs : 0.0,
        r->used_relative ? 1 : 0,
        r->used_fallback ? 1 : 0,
        (long long)(r->got_start && r->got_end ? (r->end_us - r->start_us) / 1000 : -1)
    );
}

/* ---- unit: median / freeze ---- */

static void test_median_and_freeze_excludes_event(void)
{
    printf("\n== unit: median + freeze excludes event windows ==\n");
    proto_pre_event_baseline_t b;
    proto_pre_event_baseline_init(&b, 15000);
    proto_pre_event_baseline_push(&b, -70.0f, 1000000);
    proto_pre_event_baseline_push(&b, -68.0f, 2000000);
    proto_pre_event_baseline_push(&b, -69.0f, 3000000);
    proto_pre_event_baseline_push(&b, -40.0f, 4000000); /* event window end */
    /* episode_start = 3000000 (window starting at 3s, ending 4s) */
    EXPECT(proto_pre_event_baseline_freeze(&b, 3000000));
    EXPECT(b.reliable);
    EXPECT(b.frozen_sample_count == 3);
    EXPECT(fabsf(b.frozen_baseline_dbfs - (-69.0f)) < 0.01f);
    printf("  median of [-70,-68,-69] = %.2f (expected -69)\n", (double)b.frozen_baseline_dbfs);
}

static void test_unreliable_with_few_samples(void)
{
    printf("\n== unit: unreliable baseline with <3 samples ==\n");
    proto_pre_event_baseline_t b;
    proto_pre_event_baseline_init(&b, 15000);
    proto_pre_event_baseline_push(&b, -60.0f, 1000000);
    proto_pre_event_baseline_push(&b, -61.0f, 2000000);
    EXPECT(!proto_pre_event_baseline_freeze(&b, 5000000));
    EXPECT(!b.reliable);
    EXPECT(b.frozen_sample_count == 2);
}

/* ---- scenarios (synthetic unless noted) ---- */

static void fill_quiet(rms_step_t *out, uint32_t start_seq, uint32_t n, float rms)
{
    for (uint32_t i = 0; i < n; ++i) {
        out[i].seq = start_seq + i;
        out[i].rms = rms;
        out[i].usable = true;
    }
}

static void scenario_quiet_transient(void)
{
    printf("\n== S1 synthetic: quiet room → transient → quiet ==\n");
    rms_step_t steps[16];
    fill_quiet(steps, 1, 5, -70.0f);
    steps[5] = (rms_step_t){6, -40.0f, true};
    steps[6] = (rms_step_t){7, -40.0f, true}; /* START at seq7 (2s min) */
    steps[7] = (rms_step_t){8, -40.0f, true};
    steps[8] = (rms_step_t){9, -70.0f, true};
    steps[9] = (rms_step_t){10, -70.0f, true};
    steps[10] = (rms_step_t){11, -70.0f, true};
    steps[11] = (rms_step_t){12, -70.0f, true};

    run_result_t abs_r =
        run_proto(steps, 12, PROTO_EXIT_MODE_ABSOLUTE, 0, 1000, 0);
    run_result_t rel4 =
        run_proto(steps, 12, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 4.0f, 1000, 1.0f);
    print_result("absolute", &abs_r);
    print_result("relative m4 h1", &rel4);
    EXPECT(abs_r.got_start && abs_r.got_end);
    EXPECT(rel4.got_start && rel4.got_end);
    EXPECT(rel4.baseline_reliable);
    EXPECT(rel4.used_relative);
}

static void scenario_ambient_minus50(void)
{
    printf("\n== S2 synthetic: ambient ~-50 (never below -55) ==\n");
    rms_step_t steps[20];
    fill_quiet(steps, 1, 5, -50.0f);
    steps[5] = (rms_step_t){6, -35.0f, true};
    steps[6] = (rms_step_t){7, -35.0f, true};
    for (int i = 7; i <= 10; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -35.0f, true};
    }
    /* return to ambient -50 */
    for (int i = 11; i < 18; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -50.0f, true};
    }

    run_result_t abs_r =
        run_proto(steps, 18, PROTO_EXIT_MODE_ABSOLUTE, 0, 1000, 0);
    run_result_t rel2 =
        run_proto(steps, 18, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 2.0f, 1000, 1.0f);
    run_result_t rel4 =
        run_proto(steps, 18, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 4.0f, 1000, 1.0f);
    run_result_t rel6 =
        run_proto(steps, 18, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 6.0f, 1000, 1.0f);
    run_result_t rel4_h2 =
        run_proto(steps, 18, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 4.0f, 2000, 1.0f);
    run_result_t rel4_h3 =
        run_proto(steps, 18, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 4.0f, 3000, 1.0f);

    print_result("absolute", &abs_r);
    print_result("relative m2 h1", &rel2);
    print_result("relative m4 h1", &rel4);
    print_result("relative m6 h1", &rel6);
    print_result("relative m4 h2", &rel4_h2);
    print_result("relative m4 h3", &rel4_h3);

    EXPECT(abs_r.got_start && !abs_r.got_end); /* stuck ACTIVE — primary bug */
    EXPECT(rel4.got_start && rel4.got_end && rel4.used_relative);
    EXPECT(rel2.got_end && rel6.got_end);
}

static void scenario_short_pause(void)
{
    printf("\n== S3 synthetic: continuous with 1s pause ==\n");
    rms_step_t steps[20];
    fill_quiet(steps, 1, 4, -65.0f);
    steps[4] = (rms_step_t){5, -38.0f, true};
    steps[5] = (rms_step_t){6, -38.0f, true};
    for (int i = 6; i <= 10; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -38.0f, true};
    }
    steps[11] = (rms_step_t){12, -65.0f, true}; /* 1s pause */
    for (int i = 12; i <= 16; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -38.0f, true};
    }
    steps[17] = (rms_step_t){18, -65.0f, true};
    steps[18] = (rms_step_t){19, -65.0f, true};
    steps[19] = (rms_step_t){20, -65.0f, true};

    run_result_t h1 =
        run_proto(steps, 20, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 4.0f, 1000, 1.0f);
    run_result_t h2 =
        run_proto(steps, 20, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 4.0f, 2000, 1.0f);
    print_result("relative m4 h1 (pause)", &h1);
    print_result("relative m4 h2 (pause)", &h2);
    /* 1s hold: may false-END on single pause window + next if still quiet — here pause is 1 window then loud resumes.
     * PENDING_EXIT starts at pause; next window loud cancels if above cancel thr. */
    EXPECT(h2.got_start);
    EXPECT(h2.got_end);
    /* With h2, single pause must not END before resume */
    EXPECT(h2.end_us >= 18 * 1000000LL);
}

static void scenario_slow_fade(void)
{
    printf("\n== S4 synthetic: slow fade to ambient ==\n");
    rms_step_t steps[24];
    fill_quiet(steps, 1, 4, -62.0f);
    steps[4] = (rms_step_t){5, -36.0f, true};
    steps[5] = (rms_step_t){6, -36.0f, true};
    const float fade[] = {-38.f, -42.f, -46.f, -50.f, -54.f, -58.f, -62.f, -62.f, -62.f};
    for (int i = 0; i < 9; ++i) {
        steps[6 + i] = (rms_step_t){(uint32_t)(7 + i), fade[i], true};
    }
    run_result_t rel =
        run_proto(steps, 15, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 4.0f, 1000, 1.0f);
    run_result_t abs_r =
        run_proto(steps, 15, PROTO_EXIT_MODE_ABSOLUTE, 0, 1000, 0);
    print_result("absolute", &abs_r);
    print_result("relative m4 h1", &rel);
    EXPECT(rel.got_end);
    EXPECT(abs_r.got_end); /* eventually crosses -55 */
}

static void scenario_long_running(void)
{
    printf("\n== S5 synthetic: long-running loud then return ==\n");
    rms_step_t steps[40];
    fill_quiet(steps, 1, 4, -66.0f);
    steps[4] = (rms_step_t){5, -30.0f, true};
    steps[5] = (rms_step_t){6, -30.0f, true};
    for (int i = 6; i < 30; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -30.0f, true};
    }
    for (int i = 30; i < 36; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -66.0f, true};
    }
    run_result_t rel =
        run_proto(steps, 36, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 4.0f, 2000, 1.0f);
    print_result("relative m4 h2", &rel);
    EXPECT(rel.got_start && rel.got_end);
    EXPECT(rel.end_us >= 32 * 1000000LL);
}

static void scenario_ambient_jump(void)
{
    printf("\n== S6 synthetic: ambient jump during event (risk) ==\n");
    rms_step_t steps[24];
    fill_quiet(steps, 1, 4, -70.0f);
    steps[4] = (rms_step_t){5, -40.0f, true};
    steps[5] = (rms_step_t){6, -40.0f, true};
    for (int i = 6; i < 12; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -40.0f, true};
    }
    /* HVAC-like jump: new ambient -48, source gone */
    for (int i = 12; i < 22; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -48.0f, true};
    }
    run_result_t rel4 =
        run_proto(steps, 22, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 4.0f, 1000, 1.0f);
    run_result_t rel6 =
        run_proto(steps, 22, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 6.0f, 1000, 1.0f);
    print_result("relative m4 (may stick)", &rel4);
    print_result("relative m6", &rel6);
    /* Document: baseline ~-70, -48 is 22 dB above → no relative END */
    EXPECT(rel4.got_start);
    EXPECT(!rel4.got_end);
}

static void scenario_invalid_during_exit(void)
{
    printf("\n== S7 synthetic: invalid windows during exit ==\n");
    rms_step_t steps[16];
    fill_quiet(steps, 1, 4, -68.0f);
    steps[4] = (rms_step_t){5, -40.0f, true};
    steps[5] = (rms_step_t){6, -40.0f, true};
    steps[6] = (rms_step_t){7, -40.0f, true};
    steps[7] = (rms_step_t){8, -68.0f, true}; /* pending exit */
    steps[8] = (rms_step_t){9, -120.0f, false}; /* invalid — must not count as quiet */
    steps[9] = (rms_step_t){10, -120.0f, false};
    steps[10] = (rms_step_t){11, -68.0f, true};
    steps[11] = (rms_step_t){12, -68.0f, true};

    run_result_t rel =
        run_proto(steps, 12, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 4.0f, 1000, 1.0f);
    print_result("relative m4 h1", &rel);
    EXPECT(rel.got_end);
    /* END should not occur at first invalid; hold resumes after usable */
    EXPECT(rel.end_us >= 11 * 1000000LL);
}

static void scenario_insufficient_baseline(void)
{
    printf("\n== S8 synthetic: insufficient pre-event history → fallback ==\n");
    rms_step_t steps[10];
    /* only 2 quiet then event */
    steps[0] = (rms_step_t){1, -60.0f, true};
    steps[1] = (rms_step_t){2, -60.0f, true};
    steps[2] = (rms_step_t){3, -40.0f, true};
    steps[3] = (rms_step_t){4, -40.0f, true};
    for (int i = 4; i < 8; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -50.0f, true}; /* ambient above -55 */
    }
    run_result_t rel =
        run_proto(steps, 8, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 4.0f, 1000, 1.0f);
    print_result("relative (expect fallback)", &rel);
    EXPECT(rel.got_start);
    EXPECT(!rel.baseline_reliable);
    /* ambient -50 never absolute-ends */
    EXPECT(!rel.got_end);
}

static void scenario_two_close_events(void)
{
    printf("\n== S9 synthetic: two close events ==\n");
    rms_step_t steps[28];
    fill_quiet(steps, 1, 4, -67.0f);
    steps[4] = (rms_step_t){5, -40.0f, true};
    steps[5] = (rms_step_t){6, -40.0f, true};
    steps[6] = (rms_step_t){7, -67.0f, true};
    steps[7] = (rms_step_t){8, -67.0f, true};
    /* second event soon — only 2 quiet after clear; may be unreliable */
    steps[8] = (rms_step_t){9, -40.0f, true};
    steps[9] = (rms_step_t){10, -40.0f, true};
    for (int i = 10; i < 16; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -50.0f, true};
    }
    /* with more quiet before second: */
    rms_step_t steps_b[28];
    fill_quiet(steps_b, 1, 4, -67.0f);
    steps_b[4] = (rms_step_t){5, -40.0f, true};
    steps_b[5] = (rms_step_t){6, -40.0f, true};
    steps_b[6] = (rms_step_t){7, -67.0f, true};
    steps_b[7] = (rms_step_t){8, -67.0f, true};
    steps_b[8] = (rms_step_t){9, -67.0f, true};
    steps_b[9] = (rms_step_t){10, -67.0f, true};
    steps_b[10] = (rms_step_t){11, -67.0f, true};
    steps_b[11] = (rms_step_t){12, -40.0f, true};
    steps_b[12] = (rms_step_t){13, -40.0f, true};
    for (int i = 13; i < 20; ++i) {
        steps_b[i] = (rms_step_t){(uint32_t)(i + 1), -67.0f, true};
    }

    run_result_t close =
        run_proto(steps, 16, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 4.0f, 1000, 1.0f);
    run_result_t spaced =
        run_proto(steps_b, 20, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 4.0f, 1000, 1.0f);
    print_result("close (2nd may fallback)", &close);
    print_result("spaced (2nd reliable)", &spaced);
    EXPECT(close.got_start);
    /* Ring cleared after first END; only 2 quiet windows → unreliable; -50 never absolute-ends. */
    EXPECT(!close.baseline_reliable);
    EXPECT(!close.got_end);
    EXPECT(spaced.got_start && spaced.got_end && spaced.baseline_reliable);
}

static void scenario_source_leaves(void)
{
    printf("\n== S10 synthetic: source leaves local range ==\n");
    /* Return to ambient -52 (above absolute −55) so only relative END works. */
    rms_step_t steps[18];
    fill_quiet(steps, 1, 4, -52.0f);
    steps[4] = (rms_step_t){5, -32.0f, true};
    steps[5] = (rms_step_t){6, -32.0f, true};
    steps[6] = (rms_step_t){7, -36.0f, true};
    steps[7] = (rms_step_t){8, -42.0f, true};
    for (int i = 8; i < 16; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -52.0f, true};
    }
    run_result_t abs_r =
        run_proto(steps, 16, PROTO_EXIT_MODE_ABSOLUTE, 0, 1000, 0);
    run_result_t rel =
        run_proto(steps, 16, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 4.0f, 1000, 1.0f);
    print_result("absolute", &abs_r);
    print_result("relative m4 h1", &rel);
    EXPECT(!abs_r.got_end); /* -52 > -55 */
    EXPECT(rel.got_end && rel.used_relative);
}

static void scenario_weak_event_immediate_end_risk(void)
{
    printf("\n== S11 synthetic: weak event near enter — immediate relative END risk ==\n");
    /*
     * Baseline ≈ -49; margin 4 → relative exit at -45 (== enter).
     * After START at -44, next windows at -44.5 may already satisfy relative END.
     * No min_contrast safeguard in prototype — document behavior.
     */
    rms_step_t steps[16];
    fill_quiet(steps, 1, 5, -49.0f);
    steps[5] = (rms_step_t){6, -44.0f, true};
    steps[6] = (rms_step_t){7, -44.0f, true}; /* START */
    steps[7] = (rms_step_t){8, -44.5f, true};
    steps[8] = (rms_step_t){9, -44.5f, true};
    steps[9] = (rms_step_t){10, -44.5f, true};
    for (int i = 10; i < 14; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -49.0f, true};
    }

    run_result_t m2 =
        run_proto(steps, 14, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 2.0f, 1000, 1.0f);
    run_result_t m4 =
        run_proto(steps, 14, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 4.0f, 1000, 1.0f);
    run_result_t m6 =
        run_proto(steps, 14, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 6.0f, 1000, 1.0f);
    print_result("relative m2", &m2);
    print_result("relative m4 (risk)", &m4);
    print_result("relative m6 (risk)", &m6);
    EXPECT(m4.got_start);
    /* With margin 4, exit thr = -45; -44.5 is NOT <= -45, so should stay until -49 */
    EXPECT(m4.got_end);
    /* margin 6: exit thr = -43; -44 <= -43 is false... -44 <= -43? false. -44.5 <= -43? false.
     * Wait: baseline -49 + 6 = -43. rms -44.5 <= -43? Yes -44.5 < -43. Immediate END risk! */
    if (m6.got_end && m6.end_us <= 9 * 1000000LL) {
        printf("  NOTE: m6 ended early @%lld ms — weak-event / large-margin hazard\n",
               (long long)(m6.end_us / 1000));
    }
}

static void scenario_log_inspired_ambient(void)
{
    printf("\n== S12 log-inspired (not raw replay): ambient ~-52 like L0 notes ==\n");
    /* Inspired by docs note rms≈-51.68; synthetic variation around that. */
    rms_step_t steps[20];
    const float amb[] = {-52.1f, -51.7f, -52.4f, -51.5f, -52.0f};
    for (int i = 0; i < 5; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), amb[i], true};
    }
    steps[5] = (rms_step_t){6, -38.0f, true};
    steps[6] = (rms_step_t){7, -36.5f, true};
    steps[7] = (rms_step_t){8, -37.0f, true};
    steps[8] = (rms_step_t){9, -39.0f, true};
    for (int i = 9; i < 16; ++i) {
        steps[i] = (rms_step_t){(uint32_t)(i + 1), -51.8f, true};
    }
    run_result_t abs_r =
        run_proto(steps, 16, PROTO_EXIT_MODE_ABSOLUTE, 0, 1000, 0);
    run_result_t rel =
        run_proto(steps, 16, PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, 4.0f, 2000, 1.5f);
    print_result("absolute", &abs_r);
    print_result("relative m4 h2", &rel);
    EXPECT(!abs_r.got_end);
    EXPECT(rel.got_end && rel.used_relative);
}

static void print_resource_estimate(void)
{
    printf("\n== Resource estimate (prototype → future firmware) ==\n");
    printf("  Static RAM: proto_pre_event_baseline_t ≈ %zu bytes\n",
           sizeof(proto_pre_event_baseline_t));
    printf("  Detector delta: proto_relative_detector_t ≈ %zu bytes (host prototype)\n",
           sizeof(proto_relative_detector_t));
    printf("  Stack: median sort uses ≤5 floats (~20 B) + call frames; no large locals\n");
    printf("  CPU/window: O(1) push; freeze once/START O(n log n) n≤5; no extra PCM\n");
    printf("  Heap: none\n");
}

int main(void)
{
    printf("proto baseline END evaluation (host-only)\n");

    test_median_and_freeze_excludes_event();
    test_unreliable_with_few_samples();

    scenario_quiet_transient();
    scenario_ambient_minus50();
    scenario_short_pause();
    scenario_slow_fade();
    scenario_long_running();
    scenario_ambient_jump();
    scenario_invalid_during_exit();
    scenario_insufficient_baseline();
    scenario_two_close_events();
    scenario_source_leaves();
    scenario_weak_event_immediate_end_risk();
    scenario_log_inspired_ambient();

    print_resource_estimate();

    if (g_failures) {
        fprintf(stderr, "\n%d failure(s)\n", g_failures);
        return 1;
    }
    printf("\nAll expectations passed.\n");
    return 0;
}
