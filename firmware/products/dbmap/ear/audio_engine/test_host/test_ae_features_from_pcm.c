/**
 * Host tests: PCM → Platform L0/L1 → AE signature packaging.
 *
 * Uses configured capture defaults (16 kHz / 1000 ms) from SP_MIC Kconfig.
 * Digital dBFS only — not calibrated SPL.
 *
 *   cc -std=c11 -Wall -Wextra -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     -I firmware/platform/audio/include \
 *     firmware/platform/audio/sp_acoustic_l0.c \
 *     firmware/platform/audio/sp_acoustic_l1.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/audio_engine/test_host/test_ae_features_from_pcm.c \
 *     -lm -o /tmp/test_ae_features_from_pcm && /tmp/test_ae_features_from_pcm
 */
#include "ae_signature.h"
#include "sp_acoustic_l0.h"
#include "sp_acoustic_l1.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Match CONFIG_SP_MIC_SAMPLE_RATE_HZ / CONFIG_SP_MIC_WINDOW_MS defaults. */
enum { FS_BITS = 24, SR = 16000, WIN_MS = 1000, N_SAMP = 16000 };
#define FS ((double)(1LL << (FS_BITS - 1)))

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void fill_tone(int32_t *pcm, size_t n, double freq_hz, double amp_fs)
{
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / (double)SR;
        const double x = amp_fs * sin(2.0 * M_PI * freq_hz * t);
        pcm[i] = (int32_t)lrint(x * FS);
    }
}

static void fill_noise(int32_t *pcm, size_t n, unsigned seed, double amp_fs)
{
    unsigned s = seed ? seed : 1u;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        const double u = ((double)(s >> 8) / 16777215.0) * 2.0 - 1.0;
        pcm[i] = (int32_t)lrint(u * amp_fs * FS);
    }
}

static void analyze_to_signature(
    const int32_t *pcm,
    size_t n,
    int64_t end_us,
    uint32_t seq,
    ae_signature_t *sig,
    sp_acoustic_l0_window_t *l0_out,
    sp_acoustic_l1_window_t *l1_out
)
{
    sp_acoustic_l0_filter_bank_t bank;
    sp_acoustic_l0_filter_bank_init(&bank, SR);
    sp_acoustic_l0_window_t l0;
    sp_acoustic_l0_analyze_window(
        &bank, pcm, n, FS_BITS, WIN_MS, end_us, &l0
    );

    sp_acoustic_l1_workspace_t ws;
    sp_acoustic_l1_workspace_init(&ws);
    sp_acoustic_l1_window_t l1;
    sp_acoustic_l1_analyze_window(
        &ws, pcm, n, FS_BITS, SR, WIN_MS, end_us, &l1
    );

    if (l0_out != NULL) {
        *l0_out = l0;
    }
    if (l1_out != NULL) {
        *l1_out = l1;
    }

    const sp_acoustic_l1_window_t *l1p =
        (l1.valid && !l1.clipped) ? &l1 : NULL;
    EXPECT(ae_signature_build(seq, &l0, l1p, NULL, NULL, false, sig));
}

static void test_configured_window_length(void)
{
    EXPECT(SR == 16000);
    EXPECT(WIN_MS == 1000);
    EXPECT(N_SAMP == (SR * WIN_MS) / 1000);
}

static void test_tone_pipeline(void)
{
    int32_t *pcm = (int32_t *)calloc((size_t)N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    if (pcm == NULL) {
        return;
    }
    fill_tone(pcm, (size_t)N_SAMP, 1000.0, 0.2);

    ae_signature_t sig;
    sp_acoustic_l0_window_t l0;
    sp_acoustic_l1_window_t l1;
    analyze_to_signature(pcm, (size_t)N_SAMP, 1000000, 1u, &sig, &l0, &l1);
    free(pcm);

    EXPECT(l0.valid);
    EXPECT(l0.sample_count == (uint32_t)N_SAMP);
    EXPECT(l0.window_ms == (uint32_t)WIN_MS);
    EXPECT(l0.rms_dbfs > SP_ACOUSTIC_L0_SILENCE_DBFS);
    EXPECT(l0.crest_db > 0.0f);

    EXPECT(l1.valid);
    /* Same gate as test_acoustic_l1 single-tone. */
    EXPECT(fabs(l1.dominant_hz - 1000.0) < 40.0);
    EXPECT(l1.flatness < 0.35f);

    EXPECT(sig.schema_version == AE_SIGNATURE_SCHEMA_VERSION);
    EXPECT(sig.window_seq == 1u);
    EXPECT(sig.window_end_mono_us == 1000000);
    EXPECT(sig.window_ms == (uint32_t)WIN_MS);
    EXPECT(fabsf(sig.rms_dbfs - l0.rms_dbfs) < 0.01f);
    EXPECT(fabsf(sig.peak_dbfs - l0.peak_dbfs) < 0.01f);
    EXPECT(fabsf(sig.crest_db - l0.crest_db) < 0.01f);
    for (uint32_t i = 0; i < AE_BAND_COUNT; i++) {
        EXPECT(fabsf(sig.band_dbfs[i] - l0.band_dbfs[i]) < 0.01f);
    }
    EXPECT(sig.spectral_valid);
    EXPECT(fabsf(sig.dominant_hz - l1.dominant_hz) < 0.01f);
    EXPECT(fabsf(sig.flatness - l1.flatness) < 0.01f);
    EXPECT((sig.flags & AE_FLAG_L1_MISSING) == 0u);
    EXPECT(sig.quality == AE_QUALITY_OK || sig.quality == AE_QUALITY_DEGRADED);
    EXPECT(sig.quality != AE_QUALITY_INVALID);
}

static void test_silence_pipeline(void)
{
    int32_t *pcm = (int32_t *)calloc((size_t)N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    if (pcm == NULL) {
        return;
    }

    ae_signature_t sig;
    sp_acoustic_l0_window_t l0;
    sp_acoustic_l1_window_t l1;
    analyze_to_signature(pcm, (size_t)N_SAMP, 2000000, 2u, &sig, &l0, &l1);
    free(pcm);

    /* Silence: L1 invalid → AE must flag L1 missing, not invent spectral OK. */
    EXPECT(!l1.valid);
    EXPECT((sig.flags & AE_FLAG_L1_MISSING) != 0u);
    EXPECT(!sig.spectral_valid);
    EXPECT(sig.quality != AE_QUALITY_OK);
}

static void test_noise_flatter_than_tone(void)
{
    int32_t *pcm = (int32_t *)calloc((size_t)N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    if (pcm == NULL) {
        return;
    }

    fill_tone(pcm, (size_t)N_SAMP, 1000.0, 0.2);
    ae_signature_t sig_tone;
    analyze_to_signature(pcm, (size_t)N_SAMP, 3000000, 3u, &sig_tone, NULL, NULL);
    EXPECT(sig_tone.spectral_valid);

    fill_noise(pcm, (size_t)N_SAMP, 42u, 0.15);
    ae_signature_t sig_noise;
    analyze_to_signature(pcm, (size_t)N_SAMP, 4000000, 4u, &sig_noise, NULL, NULL);
    free(pcm);

    EXPECT(sig_noise.spectral_valid);
    /* Relative comparison only — same processing, no absolute flatness claim. */
    EXPECT(sig_noise.flatness > sig_tone.flatness);
}

static void test_invalid_l0_and_empty_pcm(void)
{
    dbmap_l0_window_t l0;
    memset(&l0, 0, sizeof(l0));
    l0.valid = false;
    l0.window_end_mono_us = 5000000;
    ae_signature_t sig;
    EXPECT(ae_signature_build(5u, &l0, NULL, NULL, NULL, false, &sig));
    EXPECT(sig.quality == AE_QUALITY_INVALID);

    memset(&l0, 0, sizeof(l0));
    l0.valid = true;
    l0.window_end_mono_us = 0; /* missing acquisition time */
    EXPECT(ae_signature_build(6u, &l0, NULL, NULL, NULL, false, &sig));
    EXPECT(sig.quality == AE_QUALITY_INVALID);

    /* Empty PCM through real L0 entry point. */
    sp_acoustic_l0_filter_bank_t bank;
    sp_acoustic_l0_filter_bank_init(&bank, SR);
    sp_acoustic_l0_window_t empty_l0;
    sp_acoustic_l0_analyze_window(&bank, NULL, 0, FS_BITS, WIN_MS, 7000000, &empty_l0);
    EXPECT(!empty_l0.valid);
    EXPECT(ae_signature_build(7u, &empty_l0, NULL, NULL, NULL, false, &sig));
    EXPECT(sig.quality == AE_QUALITY_INVALID);

    EXPECT(!ae_signature_build(0u, &empty_l0, NULL, NULL, NULL, false, &sig));
}

int main(void)
{
    test_configured_window_length();
    test_tone_pipeline();
    test_silence_pipeline();
    test_noise_flatter_than_tone();
    test_invalid_l0_and_empty_pcm();
    if (g_failures != 0) {
        fprintf(stderr, "test_ae_features_from_pcm: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ae_features_from_pcm: ok\n");
    return 0;
}
