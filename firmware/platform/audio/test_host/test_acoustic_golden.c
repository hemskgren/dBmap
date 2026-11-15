/**
 * Golden equivalence: Platform sp_acoustic_* vs pre-migration frozen fixtures.
 *
 * Tolerances (explained):
 * - ABS_EPS 1e-4: absolute floor for near-zero values (flux/env often ~0; float
 *   noise below this is not meaningful for dBFS/Hz features).
 * - REL_EPS 1e-5: relative tolerance for nonzero values — tighter than the L2
 *   host survey provisional bounds (0.005/0.05); migration must be bit-stable
 *   for these deterministic synthetic PCM cases.
 *
 *   cc -std=c11 -O2 -I firmware/platform/audio/include \
 *     -I firmware/platform/audio/test_host/fixtures \
 *     firmware/platform/audio/sp_acoustic_l0.c \
 *     firmware/platform/audio/sp_acoustic_l1.c \
 *     firmware/platform/audio/sp_acoustic_l2.c \
 *     firmware/platform/audio/test_host/test_acoustic_golden.c -lm \
 *     -o /tmp/test_acoustic_golden && /tmp/test_acoustic_golden
 */
#include "sp_acoustic_l0.h"
#include "sp_acoustic_l1.h"
#include "sp_acoustic_l2.h"
#include "sp_acoustic_golden.h"

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

static const float ABS_EPS = 1.0e-4f;
static const float REL_EPS = 1.0e-5f;

static int near_eq(float got, float exp)
{
    const float a = fabsf(got - exp);
    const float s = fmaxf(fabsf(exp), 1.0f);
    return a <= ABS_EPS || a <= REL_EPS * s;
}

static void expect_f(const char *label, float got, float exp)
{
    if (!near_eq(got, exp)) {
        fprintf(
            stderr,
            "FAIL %s: got=%.9g exp=%.9g abs=%.3g rel=%.3g\n",
            label,
            got,
            exp,
            (double)fabsf(got - exp),
            (double)(fabsf(exp) > 0 ? fabsf(got - exp) / fabsf(exp) : 0)
        );
        g_failures++;
    }
}

static void fill_tone(int32_t *pcm, size_t n, double hz, double amp)
{
    const double fs = (double)((1 << (SP_GOLDEN_FS_BITS - 1)) - 1);
    for (size_t i = 0; i < n; i++) {
        double s = amp * sin(2.0 * M_PI * hz * (double)i / (double)SP_GOLDEN_SR_HZ);
        pcm[i] = (int32_t)lrint(s * fs);
    }
}

static void fill_noise(int32_t *pcm, size_t n, unsigned seed)
{
    const double fs = (double)((1 << (SP_GOLDEN_FS_BITS - 1)) - 1);
    unsigned x = seed;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        double u = ((double)(x & 0xffffffu) / (double)0xffffffu) * 2.0 - 1.0;
        pcm[i] = (int32_t)lrint(0.2 * u * fs);
    }
}

static void run_case(
    const char *name,
    int32_t *pcm,
    sp_acoustic_l0_filter_bank_t *bank,
    sp_acoustic_l1_workspace_t *ws,
    int64_t t_end
)
{
    sp_acoustic_l0_window_t l0;
    sp_acoustic_l1_window_t l1;
    sp_acoustic_l2_window_t l2;
    sp_acoustic_l0_analyze_window(
        bank, pcm, SP_GOLDEN_N, SP_GOLDEN_FS_BITS, 1000, t_end, &l0
    );
    sp_acoustic_l2_analyze_window(
        ws,
        pcm,
        SP_GOLDEN_N,
        SP_GOLDEN_FS_BITS,
        SP_GOLDEN_SR_HZ,
        1000,
        t_end,
        &l1,
        &l2
    );
    EXPECT(l0.valid);
    EXPECT(l1.valid);
    (void)name;
    (void)l2;
}

int main(void)
{
    int32_t *pcm = calloc((size_t)SP_GOLDEN_N, sizeof(int32_t));
    EXPECT(pcm != NULL);
    sp_acoustic_l0_filter_bank_t bank;
    sp_acoustic_l1_workspace_t ws;
    sp_acoustic_l0_filter_bank_init(&bank, SP_GOLDEN_SR_HZ);
    sp_acoustic_l1_workspace_init(&ws);

    fill_tone(pcm, SP_GOLDEN_N, 1000.0, 0.25);
    {
        sp_acoustic_l0_window_t l0;
        sp_acoustic_l1_window_t l1;
        sp_acoustic_l2_window_t l2;
        sp_acoustic_l0_analyze_window(
            &bank, pcm, SP_GOLDEN_N, SP_GOLDEN_FS_BITS, 1000, 1000000LL, &l0
        );
        sp_acoustic_l2_analyze_window(
            &ws,
            pcm,
            SP_GOLDEN_N,
            SP_GOLDEN_FS_BITS,
            SP_GOLDEN_SR_HZ,
            1000,
            1000000LL,
            &l1,
            &l2
        );
        expect_f("tone_l0_rms", l0.rms_dbfs, SP_GOLDEN_TONE_L0_RMS);
        expect_f("tone_l0_peak", l0.peak_dbfs, SP_GOLDEN_TONE_L0_PEAK);
        expect_f("tone_l0_crest", l0.crest_db, SP_GOLDEN_TONE_L0_CREST);
        expect_f("tone_l0_b0", l0.band_dbfs[0], SP_GOLDEN_TONE_L0_BAND[0]);
        expect_f("tone_l0_b1", l0.band_dbfs[1], SP_GOLDEN_TONE_L0_BAND[1]);
        expect_f("tone_l0_b2", l0.band_dbfs[2], SP_GOLDEN_TONE_L0_BAND[2]);
        expect_f("tone_l0_b3", l0.band_dbfs[3], SP_GOLDEN_TONE_L0_BAND[3]);
        EXPECT(l0.hl_valid == (SP_GOLDEN_TONE_L0_HL_VALID != 0));
        expect_f("tone_l0_hl", l0.hl_db, SP_GOLDEN_TONE_L0_HL);
        expect_f("tone_l1_cent", l1.centroid_hz, SP_GOLDEN_TONE_L1_CENT);
        expect_f("tone_l1_bw", l1.bandwidth_hz, SP_GOLDEN_TONE_L1_BW);
        expect_f("tone_l1_dom", l1.dominant_hz, SP_GOLDEN_TONE_L1_DOM);
        expect_f("tone_l1_flat", l1.flatness, SP_GOLDEN_TONE_L1_FLAT);
        EXPECT(l2.eligible == (SP_GOLDEN_TONE_L2_ELIG != 0));
        expect_f("tone_l2_flux", l2.spectral_flux, SP_GOLDEN_TONE_L2_FLUX);
        expect_f("tone_l2_env", l2.envelope_cv, SP_GOLDEN_TONE_L2_ENV);
    }

    fill_tone(pcm, SP_GOLDEN_N, 250.0, 0.5);
    {
        sp_acoustic_l0_window_t l0;
        sp_acoustic_l1_window_t l1;
        sp_acoustic_l2_window_t l2;
        sp_acoustic_l0_analyze_window(
            &bank, pcm, SP_GOLDEN_N, SP_GOLDEN_FS_BITS, 1000, 2000000LL, &l0
        );
        sp_acoustic_l2_analyze_window(
            &ws,
            pcm,
            SP_GOLDEN_N,
            SP_GOLDEN_FS_BITS,
            SP_GOLDEN_SR_HZ,
            1000,
            2000000LL,
            &l1,
            &l2
        );
        expect_f("t250_l0_rms", l0.rms_dbfs, SP_GOLDEN_T250_L0_RMS);
        expect_f("t250_l0_peak", l0.peak_dbfs, SP_GOLDEN_T250_L0_PEAK);
        expect_f("t250_l0_crest", l0.crest_db, SP_GOLDEN_T250_L0_CREST);
        expect_f("t250_l1_cent", l1.centroid_hz, SP_GOLDEN_T250_L1_CENT);
        expect_f("t250_l1_dom", l1.dominant_hz, SP_GOLDEN_T250_L1_DOM);
        expect_f("t250_l2_flux", l2.spectral_flux, SP_GOLDEN_T250_L2_FLUX);
        expect_f("t250_l2_env", l2.envelope_cv, SP_GOLDEN_T250_L2_ENV);
    }

    fill_noise(pcm, SP_GOLDEN_N, 42u);
    {
        sp_acoustic_l0_window_t l0;
        sp_acoustic_l1_window_t l1;
        sp_acoustic_l2_window_t l2;
        sp_acoustic_l0_analyze_window(
            &bank, pcm, SP_GOLDEN_N, SP_GOLDEN_FS_BITS, 1000, 3000000LL, &l0
        );
        sp_acoustic_l2_analyze_window(
            &ws,
            pcm,
            SP_GOLDEN_N,
            SP_GOLDEN_FS_BITS,
            SP_GOLDEN_SR_HZ,
            1000,
            3000000LL,
            &l1,
            &l2
        );
        expect_f("noise_l0_rms", l0.rms_dbfs, SP_GOLDEN_NOISE_L0_RMS);
        expect_f("noise_l0_peak", l0.peak_dbfs, SP_GOLDEN_NOISE_L0_PEAK);
        expect_f("noise_l0_crest", l0.crest_db, SP_GOLDEN_NOISE_L0_CREST);
        expect_f("noise_l1_cent", l1.centroid_hz, SP_GOLDEN_NOISE_L1_CENT);
        expect_f("noise_l1_flat", l1.flatness, SP_GOLDEN_NOISE_L1_FLAT);
        expect_f("noise_l2_flux", l2.spectral_flux, SP_GOLDEN_NOISE_L2_FLUX);
        expect_f("noise_l2_env", l2.envelope_cv, SP_GOLDEN_NOISE_L2_ENV);
    }

    /* EAR mic0 adapter path: same Platform analyze APIs used by runtime. */
    run_case("mic0_adapter_smoke", pcm, &bank, &ws, 4000000LL);

    free(pcm);
    if (g_failures != 0) {
        fprintf(stderr, "%d golden failures\n", g_failures);
        return 1;
    }
    puts("ok acoustic golden equivalence");
    return 0;
}
