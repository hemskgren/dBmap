/**
 * Host unit tests for Step 2 Phase A L0 acoustic measurements.
 *
 * From repo root:
 *   cc -std=c11 -Wall -Wextra -O2 -g \
 *     -I firmware/ear_components/ear_mic/include \
 *     firmware/ear_components/ear_mic/acoustic_l0.c \
 *     firmware/ear_components/ear_mic/test_host/test_acoustic_l0.c \
 *     -lm -o /tmp/test_dbmap_acoustic_l0 && /tmp/test_dbmap_acoustic_l0
 */
#include "dbmap_acoustic_l0.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define EXPECT_NEAR(a, b, tol)                                                 \
    do {                                                                       \
        double _a = (double)(a);                                               \
        double _b = (double)(b);                                               \
        if (!(fabs(_a - _b) <= (tol))) {                                       \
            fprintf(                                                           \
                stderr,                                                        \
                "FAIL %s:%d: |%g - %g| > %g\n",                                \
                __FILE__,                                                      \
                __LINE__,                                                      \
                _a,                                                            \
                _b,                                                            \
                (double)(tol)                                                  \
            );                                                                 \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

enum { FS_BITS = 24, SR = 16000, WIN_MS = 1000, N_SAMP = 16000 };

static int32_t *make_sine(double freq_hz, double amp_fs, size_t n)
{
    int32_t *buf = (int32_t *)calloc(n, sizeof(int32_t));
    if (!buf) {
        return NULL;
    }
    const double fs = (double)(1LL << (FS_BITS - 1));
    for (size_t i = 0; i < n; ++i) {
        double x = amp_fs * sin(2.0 * M_PI * freq_hz * (double)i / (double)SR);
        if (x > 0.999) {
            x = 0.999;
        }
        if (x < -0.999) {
            x = -0.999;
        }
        buf[i] = (int32_t)lrint(x * fs);
    }
    return buf;
}

static int32_t *make_sum2(double f0, double a0, double f1, double a1, size_t n)
{
    int32_t *buf = (int32_t *)calloc(n, sizeof(int32_t));
    if (!buf) {
        return NULL;
    }
    const double fs = (double)(1LL << (FS_BITS - 1));
    for (size_t i = 0; i < n; ++i) {
        double x = a0 * sin(2.0 * M_PI * f0 * (double)i / (double)SR)
            + a1 * sin(2.0 * M_PI * f1 * (double)i / (double)SR);
        if (x > 0.999) {
            x = 0.999;
        }
        if (x < -0.999) {
            x = -0.999;
        }
        buf[i] = (int32_t)lrint(x * fs);
    }
    return buf;
}

static int dominant_band(const dbmap_l0_window_t *w)
{
    int best = 0;
    for (int i = 1; i < DBMAP_L0_BAND_COUNT; ++i) {
        if (w->band_energy_linear[i] > w->band_energy_linear[best]) {
            best = i;
        }
    }
    return best;
}

static void test_band_plan_edges(void)
{
    EXPECT_NEAR(dbmap_l0_band_low_hz(DBMAP_L0_BAND_B0), 20.0, 1e-6);
    EXPECT_NEAR(dbmap_l0_band_high_hz(DBMAP_L0_BAND_B0), 250.0, 1e-6);
    EXPECT_NEAR(dbmap_l0_band_low_hz(DBMAP_L0_BAND_B3), 4000.0, 1e-6);
    EXPECT_NEAR(dbmap_l0_band_high_hz(DBMAP_L0_BAND_B3), 8000.0, 1e-6);
}

static void test_tone_lands_in_expected_band(void)
{
    dbmap_l0_filter_bank_t bank;
    dbmap_l0_filter_bank_init(&bank, SR);
    struct {
        double freq;
        int band;
    } cases[] = {
        {100.0, DBMAP_L0_BAND_B0},
        {500.0, DBMAP_L0_BAND_B1},
        {2000.0, DBMAP_L0_BAND_B2},
        {5000.0, DBMAP_L0_BAND_B3},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        int32_t *pcm = make_sine(cases[c].freq, 0.25, N_SAMP);
        EXPECT(pcm != NULL);
        dbmap_l0_window_t w;
        dbmap_l0_analyze_window(&bank, pcm, N_SAMP, FS_BITS, WIN_MS, 1000000, &w);
        EXPECT(w.valid);
        EXPECT(dominant_band(&w) == cases[c].band);
        /* Dominant band should hold a clear majority of band energy. */
        double tot = 0.0;
        for (int b = 0; b < DBMAP_L0_BAND_COUNT; ++b) {
            tot += w.band_energy_linear[b];
        }
        EXPECT(tot > 0.0);
        double frac = w.band_energy_linear[cases[c].band] / tot;
        EXPECT(frac > 0.45);
        free(pcm);
    }
}

static void test_mixed_tones_combined_energy(void)
{
    dbmap_l0_filter_bank_t bank;
    dbmap_l0_filter_bank_init(&bank, SR);
    /* 100 Hz + 5 kHz — combined scene, energy in B0 and B3. */
    int32_t *pcm = make_sum2(100.0, 0.2, 5000.0, 0.2, N_SAMP);
    EXPECT(pcm != NULL);
    dbmap_l0_window_t w;
    dbmap_l0_analyze_window(&bank, pcm, N_SAMP, FS_BITS, WIN_MS, 2000000, &w);
    EXPECT(w.valid);
    double tot = 0.0;
    for (int b = 0; b < DBMAP_L0_BAND_COUNT; ++b) {
        tot += w.band_energy_linear[b];
    }
    EXPECT(tot > 0.0);
    double f0 = w.band_energy_linear[0] / tot;
    double f3 = w.band_energy_linear[3] / tot;
    EXPECT(f0 > 0.2);
    EXPECT(f3 > 0.2);
    free(pcm);
}

static void test_silence_fractions_null(void)
{
    dbmap_l0_filter_bank_t bank;
    dbmap_l0_filter_bank_init(&bank, SR);
    int32_t *pcm = (int32_t *)calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    dbmap_l0_window_t w;
    dbmap_l0_analyze_window(&bank, pcm, N_SAMP, FS_BITS, WIN_MS, 1000, &w);
    EXPECT(w.valid);
    EXPECT(w.rms_dbfs <= -100.0f);

    float hist_rms[8], hist_hl[8], hist_crest[8];
    uint8_t hist_hl_ok[8], hist_crest_ok[8];
    int64_t hist_t[8];
    dbmap_l0_interval_accum_t acc;
    dbmap_l0_interval_accum_init(
        &acc,
        SR,
        WIN_MS,
        0,
        0,
        5000000,
        hist_rms,
        hist_hl,
        hist_hl_ok,
        hist_t,
        hist_crest,
        hist_crest_ok,
        8
    );
    dbmap_l0_interval_accum_add(&acc, &w);
    dbmap_l0_interval_t iv;
    dbmap_l0_interval_accum_finalize(&acc, &iv);
    EXPECT(iv.window_count == 1);
    EXPECT(!iv.has_band_fractions);
    free(pcm);
}

static void test_crest_and_impulse(void)
{
    dbmap_l0_filter_bank_t bank;
    dbmap_l0_filter_bank_init(&bank, SR);
    int32_t *pcm = (int32_t *)calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    const double fs = (double)(1LL << (FS_BITS - 1));
    /* Mostly quiet with a single spike → high crest. */
    pcm[N_SAMP / 2] = (int32_t)lrint(0.9 * fs);
    dbmap_l0_window_t w;
    dbmap_l0_analyze_window(&bank, pcm, N_SAMP, FS_BITS, WIN_MS, 1000, &w);
    EXPECT(w.valid);
    EXPECT(w.crest_db > DBMAP_L0_CREST_THRESHOLD_DB);
    EXPECT(!w.clipped);
    free(pcm);
}

static void test_clipped_excluded_from_t3(void)
{
    dbmap_l0_filter_bank_t bank;
    dbmap_l0_filter_bank_init(&bank, SR);
    int32_t *pcm = make_sine(1000.0, 0.999, N_SAMP);
    EXPECT(pcm != NULL);
    /* Force full-scale samples */
    const int32_t fs = (int32_t)((1LL << (FS_BITS - 1)) - 1);
    for (int i = 0; i < 100; ++i) {
        pcm[i] = fs;
    }
    dbmap_l0_window_t w;
    dbmap_l0_analyze_window(&bank, pcm, N_SAMP, FS_BITS, WIN_MS, 1000, &w);
    EXPECT(w.valid);
    EXPECT(w.clipped);

    float hist_rms[4], hist_hl[4], hist_crest[4];
    uint8_t hist_hl_ok[4], hist_crest_ok[4];
    int64_t hist_t[4];
    dbmap_l0_interval_accum_t acc;
    dbmap_l0_interval_accum_init(
        &acc, SR, WIN_MS, 0, 0, 10000000, hist_rms, hist_hl, hist_hl_ok, hist_t, hist_crest,
        hist_crest_ok, 4
    );
    dbmap_l0_interval_accum_add(&acc, &w);
    dbmap_l0_interval_t iv;
    dbmap_l0_interval_accum_finalize(&acc, &iv);
    EXPECT(iv.clipped_window_count == 1);
    EXPECT(iv.n_crest == 0);
    EXPECT(!iv.has_t3);
    free(pcm);
}

static dbmap_l0_window_t stub_window(int64_t end_us, float rms_dbfs)
{
    dbmap_l0_window_t w;
    memset(&w, 0, sizeof(w));
    w.valid = true;
    w.window_end_mono_us = end_us;
    w.window_ms = WIN_MS;
    w.sample_count = (uint32_t)N_SAMP;
    w.rms_dbfs = rms_dbfs;
    w.peak_dbfs = rms_dbfs + 3.0f;
    w.crest_db = 3.0f;
    for (int b = 0; b < DBMAP_L0_BAND_COUNT; ++b) {
        w.band_energy_linear[b] = 1.0e-6;
        w.band_dbfs[b] = -60.0f;
    }
    w.hl_valid = true;
    w.hl_db = 0.0f;
    return w;
}

static uint32_t count_in_interval(
    const dbmap_l0_window_t *wins,
    uint32_t n,
    int64_t start_us,
    int64_t end_us
)
{
    float hist_rms[16], hist_hl[16], hist_crest[16];
    uint8_t hist_hl_ok[16], hist_crest_ok[16];
    int64_t hist_t[16];
    dbmap_l0_interval_accum_t acc;
    dbmap_l0_interval_accum_init(
        &acc,
        SR,
        WIN_MS,
        0,
        start_us,
        end_us,
        hist_rms,
        hist_hl,
        hist_hl_ok,
        hist_t,
        hist_crest,
        hist_crest_ok,
        16
    );
    for (uint32_t i = 0; i < n; ++i) {
        dbmap_l0_interval_accum_add(&acc, &wins[i]);
    }
    dbmap_l0_interval_t iv;
    dbmap_l0_interval_accum_finalize(&acc, &iv);
    return iv.window_count;
}

static void test_membership_exclusive_start_inclusive_end(void)
{
    const int64_t s = 10000000;
    const int64_t e = 15100000;
    EXPECT(!dbmap_l0_window_in_interval(s, s, e)); /* start exclusive */
    EXPECT(dbmap_l0_window_in_interval(s + 1, s, e));
    EXPECT(dbmap_l0_window_in_interval(e, s, e)); /* end inclusive (closing) */
    EXPECT(!dbmap_l0_window_in_interval(e + 1, s, e));
    EXPECT(!dbmap_l0_window_in_interval(s - 1, s, e));
}

/** START: episode_start = first window's start; end = closing measured_at. */
static void test_start_includes_closing_window(void)
{
    /* First enter window spans [9.0s, 10.0s]; second closes START at 11.0s. */
    const int64_t episode_start = 9000000; /* window start of first enter */
    const int64_t close_us = 11000000;
    dbmap_l0_window_t wins[3] = {
        stub_window(10000000, -40.0f),
        stub_window(11000000, -38.0f), /* closing */
        stub_window(12000000, -37.0f), /* after START — not in START */
    };
    EXPECT(count_in_interval(wins, 3, episode_start, close_us) == 2);
    EXPECT(dbmap_l0_window_in_interval(11000000, episode_start, close_us));
    EXPECT(!dbmap_l0_window_in_interval(episode_start, episode_start, close_us));
}

/**
 * PERIODIC chain: next.start == prev.end. Closing of A in A only; closing of B in B only.
 */
static void test_periodic_chain_no_gap_or_double_count(void)
{
    const int64_t t0 = 10000000;
    const int64_t t1 = 15100000;
    const int64_t t2 = 20200000;
    dbmap_l0_window_t wins[10] = {
        stub_window(11020000, -50.0f),
        stub_window(12040000, -49.0f),
        stub_window(13060000, -48.0f),
        stub_window(14080000, -47.0f),
        stub_window(15100000, -46.0f), /* closes PERIODIC A */
        stub_window(16120000, -45.0f),
        stub_window(17140000, -44.0f),
        stub_window(18160000, -43.0f),
        stub_window(19180000, -42.0f),
        stub_window(20200000, -41.0f), /* closes PERIODIC B */
    };
    EXPECT(count_in_interval(wins, 10, t0, t1) == 5);
    EXPECT(count_in_interval(wins, 10, t1, t2) == 5);
    /* Boundary window belongs to exactly one interval. */
    EXPECT(dbmap_l0_window_in_interval(t1, t0, t1));
    EXPECT(!dbmap_l0_window_in_interval(t1, t1, t2));
    EXPECT(dbmap_l0_window_in_interval(t2, t1, t2));
}

/** END / forced END: last usable close included; previous PERIODIC close excluded. */
static void test_end_and_forced_end_include_last_usable(void)
{
    const int64_t prev_emit = 15100000;
    const int64_t last_valid = 16120000;
    dbmap_l0_window_t wins[3] = {
        stub_window(15100000, -46.0f), /* previous close — not in END */
        stub_window(15600000, -50.0f),
        stub_window(16120000, -52.0f), /* last usable / forced-END boundary */
    };
    EXPECT(count_in_interval(wins, 3, prev_emit, last_valid) == 2);
    EXPECT(dbmap_l0_window_in_interval(last_valid, prev_emit, last_valid));
    EXPECT(!dbmap_l0_window_in_interval(prev_emit, prev_emit, last_valid));
}

static void test_invalid_window_skipped(void)
{
    dbmap_l0_window_t bad = stub_window(2000000, -40.0f);
    bad.valid = false;
    dbmap_l0_window_t good = stub_window(3000000, -41.0f);
    dbmap_l0_window_t wins[2] = {bad, good};
    EXPECT(count_in_interval(wins, 2, 0, 5000000) == 1);
}

static void test_interval_membership_and_stats(void)
{
    dbmap_l0_filter_bank_t bank;
    dbmap_l0_filter_bank_init(&bank, SR);
    int32_t *quiet = make_sine(500.0, 0.05, N_SAMP);
    int32_t *loud = make_sine(500.0, 0.25, N_SAMP);
    EXPECT(quiet && loud);

    dbmap_l0_window_t w0, w1, w2, w_out, w_close;
    dbmap_l0_analyze_window(&bank, quiet, N_SAMP, FS_BITS, WIN_MS, 1000000, &w0);
    dbmap_l0_analyze_window(&bank, loud, N_SAMP, FS_BITS, WIN_MS, 2000000, &w1);
    dbmap_l0_analyze_window(&bank, quiet, N_SAMP, FS_BITS, WIN_MS, 3000000, &w2);
    dbmap_l0_analyze_window(&bank, quiet, N_SAMP, FS_BITS, WIN_MS, 5000000, &w_close);
    dbmap_l0_analyze_window(&bank, loud, N_SAMP, FS_BITS, WIN_MS, 6000000, &w_out);

    /* Interval (0, 5s]: includes w0..w2 and closing @5s; excludes @6s. */
    EXPECT(dbmap_l0_window_in_interval(2000000, 0, 5000000));
    EXPECT(dbmap_l0_window_in_interval(5000000, 0, 5000000));
    EXPECT(!dbmap_l0_window_in_interval(0, 0, 5000000));
    EXPECT(!dbmap_l0_window_in_interval(6000000, 0, 5000000));

    float hist_rms[8], hist_hl[8], hist_crest[8];
    uint8_t hist_hl_ok[8], hist_crest_ok[8];
    int64_t hist_t[8];
    dbmap_l0_interval_accum_t acc;
    dbmap_l0_interval_accum_init(
        &acc, SR, WIN_MS, 0, 0, 5000000, hist_rms, hist_hl, hist_hl_ok, hist_t,
        hist_crest, hist_crest_ok, 8
    );
    dbmap_l0_interval_accum_add(&acc, &w0);
    dbmap_l0_interval_accum_add(&acc, &w1);
    dbmap_l0_interval_accum_add(&acc, &w2);
    dbmap_l0_interval_accum_add(&acc, &w_close);
    dbmap_l0_interval_accum_add(&acc, &w_out); /* outside */
    dbmap_l0_interval_t iv;
    dbmap_l0_interval_accum_finalize(&acc, &iv);

    EXPECT(iv.window_count == 4);
    EXPECT(iv.has_rms_stats);
    EXPECT(iv.rms_max_dbfs > iv.rms_mean_dbfs);
    EXPECT(iv.rms_min_dbfs < iv.rms_mean_dbfs);
    EXPECT(iv.has_t1);
    EXPECT(iv.rms_range_db > 0.5f);
    EXPECT(iv.has_band_fractions);
    EXPECT_NEAR(iv.band_fraction[0] + iv.band_fraction[1] + iv.band_fraction[2]
            + iv.band_fraction[3],
        1.0,
        1e-5);

    free(quiet);
    free(loud);
}

static void test_t2_positive_slope(void)
{
    dbmap_l0_filter_bank_t bank;
    dbmap_l0_filter_bank_init(&bank, SR);
    float hist_rms[8], hist_hl[8], hist_crest[8];
    uint8_t hist_hl_ok[8], hist_crest_ok[8];
    int64_t hist_t[8];
    dbmap_l0_interval_accum_t acc;
    dbmap_l0_interval_accum_init(
        &acc, SR, WIN_MS, 0, 0, 10000000, hist_rms, hist_hl, hist_hl_ok, hist_t, hist_crest,
        hist_crest_ok, 8
    );

    const double amps[] = {0.02, 0.05, 0.10, 0.20};
    for (int i = 0; i < 4; ++i) {
        int32_t *pcm = make_sine(1000.0, amps[i], N_SAMP);
        dbmap_l0_window_t w;
        dbmap_l0_analyze_window(
            &bank, pcm, N_SAMP, FS_BITS, WIN_MS, (int64_t)(i + 1) * 1000000LL, &w
        );
        dbmap_l0_interval_accum_add(&acc, &w);
        free(pcm);
    }
    dbmap_l0_interval_t iv;
    dbmap_l0_interval_accum_finalize(&acc, &iv);
    EXPECT(iv.window_count == 4);
    EXPECT(iv.has_t2);
    EXPECT(iv.rms_slope_db_per_s > 1.0f);
}

static void test_band_mean_uses_linear_average(void)
{
    dbmap_l0_filter_bank_t bank;
    dbmap_l0_filter_bank_init(&bank, SR);
    /* Two windows: very different levels same band — mean dB via linear avg
     * should not equal arithmetic mean of dB. */
    int32_t *a = make_sine(500.0, 0.02, N_SAMP);
    int32_t *b = make_sine(500.0, 0.20, N_SAMP);
    dbmap_l0_window_t wa, wb;
    dbmap_l0_analyze_window(&bank, a, N_SAMP, FS_BITS, WIN_MS, 1000000, &wa);
    dbmap_l0_analyze_window(&bank, b, N_SAMP, FS_BITS, WIN_MS, 2000000, &wb);

    float hist_rms[4], hist_hl[4], hist_crest[4];
    uint8_t hist_hl_ok[4], hist_crest_ok[4];
    int64_t hist_t[4];
    dbmap_l0_interval_accum_t acc;
    dbmap_l0_interval_accum_init(
        &acc, SR, WIN_MS, 0, 0, 5000000, hist_rms, hist_hl, hist_hl_ok, hist_t, hist_crest,
        hist_crest_ok, 4
    );
    dbmap_l0_interval_accum_add(&acc, &wa);
    dbmap_l0_interval_accum_add(&acc, &wb);
    dbmap_l0_interval_t iv;
    dbmap_l0_interval_accum_finalize(&acc, &iv);

    const int k = DBMAP_L0_BAND_B1;
    const double mean_lin =
        0.5 * (wa.band_energy_linear[k] + wb.band_energy_linear[k]);
    const float expect_db = (float)(10.0 * log10(mean_lin));
    EXPECT_NEAR(iv.band_mean_dbfs[k], expect_db, 0.15);
    const float arith_db = 0.5f * (wa.band_dbfs[k] + wb.band_dbfs[k]);
    EXPECT(fabsf(iv.band_mean_dbfs[k] - arith_db) > 0.5f);

    free(a);
    free(b);
}

static void test_hl_mean_from_linear_sums(void)
{
    dbmap_l0_filter_bank_t bank;
    dbmap_l0_filter_bank_init(&bank, SR);

    /* Low-band vs high-band dominant windows → HL mean from ΣE, not mean of dB. */
    int32_t *low = make_sine(100.0, 0.15, N_SAMP);
    int32_t *high = make_sine(5000.0, 0.15, N_SAMP);
    EXPECT(low && high);
    dbmap_l0_window_t w0, w1;
    dbmap_l0_analyze_window(&bank, low, N_SAMP, FS_BITS, WIN_MS, 1000000, &w0);
    dbmap_l0_analyze_window(&bank, high, N_SAMP, FS_BITS, WIN_MS, 2000000, &w1);

    float hist_rms[4], hist_hl[4], hist_crest[4];
    uint8_t hist_hl_ok[4], hist_crest_ok[4];
    int64_t hist_t[4];
    dbmap_l0_interval_accum_t acc;
    dbmap_l0_interval_accum_init(
        &acc, SR, WIN_MS, 0, 0, 5000000, hist_rms, hist_hl, hist_hl_ok, hist_t, hist_crest,
        hist_crest_ok, 4
    );
    dbmap_l0_interval_accum_add(&acc, &w0);
    dbmap_l0_interval_accum_add(&acc, &w1);
    dbmap_l0_interval_t iv;
    dbmap_l0_interval_accum_finalize(&acc, &iv);

    EXPECT(iv.has_hl_mean);
    double e_low = acc.sum_band_energy[0] + acc.sum_band_energy[1];
    double e_high = acc.sum_band_energy[2] + acc.sum_band_energy[3];
    float expect = (float)(10.0 * log10(e_high / e_low));
    EXPECT_NEAR(iv.hl_mean_db, expect, 1e-3);
    /* Prefer linear-Σ formula over arithmetic mean of per-window HL_db. */
    if (w0.hl_valid && w1.hl_valid) {
        float arith = 0.5f * (w0.hl_db + w1.hl_db);
        EXPECT(fabsf(iv.hl_mean_db - expect) <= fabsf(iv.hl_mean_db - arith) + 1e-3f);
    }

    free(low);
    free(high);
}

static void test_hl_mean_null_on_silence(void)
{
    dbmap_l0_window_t w = stub_window(1000000, -120.0f);
    for (int b = 0; b < DBMAP_L0_BAND_COUNT; ++b) {
        w.band_energy_linear[b] = 0.0;
    }
    w.hl_valid = false;
    float hist_rms[4], hist_hl[4], hist_crest[4];
    uint8_t hist_hl_ok[4], hist_crest_ok[4];
    int64_t hist_t[4];
    dbmap_l0_interval_accum_t acc;
    dbmap_l0_interval_accum_init(
        &acc, SR, WIN_MS, 0, 0, 5000000, hist_rms, hist_hl, hist_hl_ok, hist_t, hist_crest,
        hist_crest_ok, 4
    );
    dbmap_l0_interval_accum_add(&acc, &w);
    dbmap_l0_interval_t iv;
    dbmap_l0_interval_accum_finalize(&acc, &iv);
    EXPECT(!iv.has_hl_mean);
}

static void test_band_fraction_hl_cross_field_consistency(void)
{
    /* All three fields must come from the same Σ linear band energies. */
    dbmap_l0_window_t w;
    memset(&w, 0, sizeof(w));
    w.valid = true;
    w.window_ms = WIN_MS;
    w.sample_count = N_SAMP;
    w.window_end_mono_us = 1000000;
    w.rms_dbfs = -40.0f;
    w.peak_dbfs = -30.0f;
    w.crest_db = 10.0f;
    /* Mean linear energies after one window (== sums). */
    w.band_energy_linear[0] = 4.0e-5;
    w.band_energy_linear[1] = 3.0e-5;
    w.band_energy_linear[2] = 2.0e-5;
    w.band_energy_linear[3] = 1.0e-5;
    w.hl_valid = true;
    w.hl_db = 0.0f; /* ignored for interval HL mean */

    float hist_rms[4], hist_hl[4], hist_crest[4];
    uint8_t hist_hl_ok[4], hist_crest_ok[4];
    int64_t hist_t[4];
    dbmap_l0_interval_accum_t acc;
    dbmap_l0_interval_accum_init(
        &acc, SR, WIN_MS, 0, 0, 5000000, hist_rms, hist_hl, hist_hl_ok, hist_t, hist_crest,
        hist_crest_ok, 4
    );
    dbmap_l0_interval_accum_add(&acc, &w);
    dbmap_l0_interval_t iv;
    dbmap_l0_interval_accum_finalize(&acc, &iv);

    EXPECT(iv.has_band_means);
    EXPECT(iv.has_band_fractions);
    EXPECT(iv.has_hl_mean);

    EXPECT_NEAR(iv.band_mean_dbfs[0], 10.0 * log10(4.0e-5), 1e-4);
    EXPECT_NEAR(iv.band_mean_dbfs[1], 10.0 * log10(3.0e-5), 1e-4);
    EXPECT_NEAR(iv.band_mean_dbfs[2], 10.0 * log10(2.0e-5), 1e-4);
    EXPECT_NEAR(iv.band_mean_dbfs[3], 10.0 * log10(1.0e-5), 1e-4);

    EXPECT_NEAR(iv.band_fraction[0], 0.4, 1e-6);
    EXPECT_NEAR(iv.band_fraction[1], 0.3, 1e-6);
    EXPECT_NEAR(iv.band_fraction[2], 0.2, 1e-6);
    EXPECT_NEAR(iv.band_fraction[3], 0.1, 1e-6);
    EXPECT_NEAR(
        iv.band_fraction[0] + iv.band_fraction[1] + iv.band_fraction[2]
            + iv.band_fraction[3],
        1.0,
        1e-6
    );

    /* Reconstruct linear means from published mean_dbfs and check fraction + HL. */
    double reconstr[4];
    double tot = 0.0;
    for (int b = 0; b < 4; ++b) {
        reconstr[b] = pow(10.0, (double)iv.band_mean_dbfs[b] / 10.0);
        tot += reconstr[b];
    }
    for (int b = 0; b < 4; ++b) {
        EXPECT_NEAR(iv.band_fraction[b], reconstr[b] / tot, 1e-5);
    }
    const double e_low = reconstr[0] + reconstr[1];
    const double e_high = reconstr[2] + reconstr[3];
    EXPECT_NEAR(iv.hl_mean_db, 10.0 * log10(e_high / e_low), 1e-4);
    EXPECT_NEAR(iv.hl_mean_db, 10.0 * log10(0.3 / 0.7), 1e-4);
}

static void test_band_fraction_hl_null_when_below_energy_floor(void)
{
    dbmap_l0_window_t w;
    memset(&w, 0, sizeof(w));
    w.valid = true;
    w.window_ms = WIN_MS;
    w.sample_count = N_SAMP;
    w.window_end_mono_us = 1000000;
    w.rms_dbfs = DBMAP_L0_SILENCE_DBFS;
    w.peak_dbfs = DBMAP_L0_SILENCE_DBFS;
    /* Below VALID_ENERGY_FLOOR * window_count for totals / HL. */
    for (int b = 0; b < DBMAP_L0_BAND_COUNT; ++b) {
        w.band_energy_linear[b] = DBMAP_L0_VALID_ENERGY_FLOOR * 0.1;
    }
    w.hl_valid = false;

    float hist_rms[2], hist_hl[2], hist_crest[2];
    uint8_t hist_hl_ok[2], hist_crest_ok[2];
    int64_t hist_t[2];
    dbmap_l0_interval_accum_t acc;
    dbmap_l0_interval_accum_init(
        &acc, SR, WIN_MS, 0, 0, 5000000, hist_rms, hist_hl, hist_hl_ok, hist_t, hist_crest,
        hist_crest_ok, 2
    );
    dbmap_l0_interval_accum_add(&acc, &w);
    dbmap_l0_interval_t iv;
    dbmap_l0_interval_accum_finalize(&acc, &iv);

    EXPECT(iv.has_band_means); /* means still published (may hit silence floor) */
    EXPECT(!iv.has_band_fractions);
    EXPECT(!iv.has_hl_mean);
}

static void test_history_overflow_clears_temporal_features(void)
{
    /* Cap=2: third associated usable window sets overflow; T1–T6 must clear. */
    float hist_rms[2], hist_hl[2], hist_crest[2];
    uint8_t hist_hl_ok[2], hist_crest_ok[2];
    int64_t hist_t[2];
    dbmap_l0_interval_accum_t acc;
    dbmap_l0_interval_accum_init(
        &acc, SR, WIN_MS, 0, 0, 10000000, hist_rms, hist_hl, hist_hl_ok, hist_t, hist_crest,
        hist_crest_ok, 2
    );

    dbmap_l0_window_t w;
    memset(&w, 0, sizeof(w));
    w.valid = true;
    w.window_ms = WIN_MS;
    w.sample_count = N_SAMP;
    w.rms_dbfs = -40.0f;
    w.peak_dbfs = -30.0f;
    w.crest_db = 10.0f;
    w.band_energy_linear[0] = 1e-4;
    w.band_energy_linear[1] = 1e-4;
    w.band_energy_linear[2] = 1e-4;
    w.band_energy_linear[3] = 1e-4;
    w.hl_valid = true;
    w.hl_db = 0.0f;

    for (int i = 0; i < 3; ++i) {
        w.window_end_mono_us = (int64_t)(i + 1) * 1000000;
        w.rms_dbfs = -40.0f - (float)i;
        dbmap_l0_interval_accum_add(&acc, &w);
    }
    EXPECT(acc.history_overflow);
    EXPECT(acc.window_count == 3);
    EXPECT(acc.hist_count == 2);

    dbmap_l0_interval_t iv;
    dbmap_l0_interval_accum_finalize(&acc, &iv);
    EXPECT(iv.history_overflow);
    EXPECT(iv.window_count == 3);
    EXPECT(iv.has_rms_stats);
    EXPECT(!iv.has_t1);
    EXPECT(!iv.has_t2);
    EXPECT(!iv.has_t3);
    EXPECT(!iv.has_t6);
}

static void test_history_capacity_boundary_exact_cap(void)
{
    /* Exactly hist_cap windows: no overflow; T1 available for N>=2. */
    enum { CAP = 4 };
    float hist_rms[CAP], hist_hl[CAP], hist_crest[CAP];
    uint8_t hist_hl_ok[CAP], hist_crest_ok[CAP];
    int64_t hist_t[CAP];
    dbmap_l0_interval_accum_t acc;
    dbmap_l0_interval_accum_init(
        &acc, SR, WIN_MS, 0, 0, 10000000, hist_rms, hist_hl, hist_hl_ok, hist_t, hist_crest,
        hist_crest_ok, CAP
    );

    dbmap_l0_window_t w;
    memset(&w, 0, sizeof(w));
    w.valid = true;
    w.window_ms = WIN_MS;
    w.sample_count = N_SAMP;
    w.peak_dbfs = -30.0f;
    w.crest_db = 8.0f;
    w.band_energy_linear[0] = 1e-4;
    w.band_energy_linear[1] = 1e-4;
    w.band_energy_linear[2] = 2e-4;
    w.band_energy_linear[3] = 2e-4;
    w.hl_valid = true;
    w.hl_db = 3.0f;

    for (int i = 0; i < CAP; ++i) {
        w.window_end_mono_us = (int64_t)(i + 1) * 1000000;
        w.rms_dbfs = -50.0f + (float)i;
        dbmap_l0_interval_accum_add(&acc, &w);
    }
    EXPECT(!acc.history_overflow);
    EXPECT(acc.hist_count == CAP);

    dbmap_l0_interval_t iv;
    dbmap_l0_interval_accum_finalize(&acc, &iv);
    EXPECT(!iv.history_overflow);
    EXPECT(iv.window_count == CAP);
    EXPECT(iv.has_t1);
    EXPECT(iv.has_t2);
}

static void test_zero_hist_cap_marks_overflow(void)
{
    dbmap_l0_interval_accum_t acc;
    dbmap_l0_interval_accum_init(
        &acc, SR, WIN_MS, 0, 0, 5000000, NULL, NULL, NULL, NULL, NULL, NULL, 0
    );
    dbmap_l0_window_t w;
    memset(&w, 0, sizeof(w));
    w.valid = true;
    w.window_ms = WIN_MS;
    w.sample_count = N_SAMP;
    w.window_end_mono_us = 1000000;
    w.rms_dbfs = -40.0f;
    w.peak_dbfs = -30.0f;
    w.crest_db = 10.0f;
    w.band_energy_linear[0] = 1e-4;
    w.band_energy_linear[1] = 1e-4;
    w.band_energy_linear[2] = 1e-4;
    w.band_energy_linear[3] = 1e-4;
    dbmap_l0_interval_accum_add(&acc, &w);
    EXPECT(acc.history_overflow);
    dbmap_l0_interval_t iv;
    dbmap_l0_interval_accum_finalize(&acc, &iv);
    EXPECT(iv.history_overflow);
    EXPECT(!iv.has_t1);
}

static void test_t3_rate_normalization(void)
{
    dbmap_l0_filter_bank_t bank;
    dbmap_l0_filter_bank_init(&bank, SR);

    int32_t *steady = make_sine(1000.0, 0.1, N_SAMP);
    int32_t *impulse = (int32_t *)calloc(N_SAMP, sizeof(int32_t));
    const double fs = (double)(1LL << (FS_BITS - 1));
    impulse[100] = (int32_t)lrint(0.85 * fs);

    dbmap_l0_window_t w_s, w_i;
    dbmap_l0_analyze_window(&bank, steady, N_SAMP, FS_BITS, WIN_MS, 1000000, &w_s);
    dbmap_l0_analyze_window(&bank, impulse, N_SAMP, FS_BITS, WIN_MS, 2000000, &w_i);
    EXPECT(w_i.crest_db >= DBMAP_L0_CREST_THRESHOLD_DB);

    float hist_rms[4], hist_hl[4], hist_crest[4];
    uint8_t hist_hl_ok[4], hist_crest_ok[4];
    int64_t hist_t[4];
    dbmap_l0_interval_accum_t acc;
    dbmap_l0_interval_accum_init(
        &acc, SR, WIN_MS, 0, 0, 5000000, hist_rms, hist_hl, hist_hl_ok, hist_t, hist_crest,
        hist_crest_ok, 4
    );
    dbmap_l0_interval_accum_add(&acc, &w_s);
    dbmap_l0_interval_accum_add(&acc, &w_i);
    dbmap_l0_interval_t iv;
    dbmap_l0_interval_accum_finalize(&acc, &iv);
    EXPECT(iv.has_t3);
    EXPECT(iv.high_crest_count == 1);
    EXPECT_NEAR(iv.high_crest_rate, 0.5, 1e-5);

    free(steady);
    free(impulse);
}

int main(void)
{
    test_band_plan_edges();
    test_tone_lands_in_expected_band();
    test_mixed_tones_combined_energy();
    test_silence_fractions_null();
    test_crest_and_impulse();
    test_clipped_excluded_from_t3();
    test_membership_exclusive_start_inclusive_end();
    test_start_includes_closing_window();
    test_periodic_chain_no_gap_or_double_count();
    test_end_and_forced_end_include_last_usable();
    test_invalid_window_skipped();
    test_interval_membership_and_stats();
    test_hl_mean_from_linear_sums();
    test_hl_mean_null_on_silence();
    test_t2_positive_slope();
    test_band_mean_uses_linear_average();
    test_band_fraction_hl_cross_field_consistency();
    test_band_fraction_hl_null_when_below_energy_floor();
    test_t3_rate_normalization();
    test_history_overflow_clears_temporal_features();
    test_history_capacity_boundary_exact_cap();
    test_zero_hist_cap_marks_overflow();

    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
