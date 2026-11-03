#include "dbmap_acoustic_l1.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { FS_BITS = 24, SR = 16000, WIN_MS = 1000, N_SAMP = 16000 };
#define FS ((double)(1LL << (FS_BITS - 1)))

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            return 1;                                                          \
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

static void fill_two_tone(
    int32_t *pcm, size_t n, double f0, double a0, double f1, double a1
)
{
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / (double)SR;
        const double x =
            a0 * sin(2.0 * M_PI * f0 * t) + a1 * sin(2.0 * M_PI * f1 * t);
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

static int test_single_tone(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    fill_tone(pcm, N_SAMP, 1000.0, 0.2);

    dbmap_l1_window_t w;
    dbmap_l1_analyze_window(&ws, pcm, N_SAMP, FS_BITS, SR, WIN_MS, 1000000, &w);
    free(pcm);

    EXPECT(w.valid);
    EXPECT(!w.clipped);
    EXPECT(w.frame_count == 15); /* 16000/1024 */
    EXPECT(fabs(w.dominant_hz - 1000.0) < 40.0);
    EXPECT(fabs(w.centroid_hz - 1000.0) < 80.0);
    EXPECT(w.bandwidth_hz < 200.0);
    EXPECT(w.flatness < 0.35);
    return 0;
}

static int test_two_tone(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    /* Louder 2000 Hz should dominate; centroid between 1000 and 2000. */
    fill_two_tone(pcm, N_SAMP, 1000.0, 0.08, 2000.0, 0.2);

    dbmap_l1_window_t w;
    dbmap_l1_analyze_window(&ws, pcm, N_SAMP, FS_BITS, SR, WIN_MS, 2000000, &w);
    free(pcm);

    EXPECT(w.valid);
    EXPECT(fabs(w.dominant_hz - 2000.0) < 50.0);
    EXPECT(w.centroid_hz > 1200.0 && w.centroid_hz < 2000.0);
    EXPECT(w.bandwidth_hz > 100.0);
    return 0;
}

static int test_white_noise(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    fill_noise(pcm, N_SAMP, 42u, 0.15);

    dbmap_l1_window_t w;
    dbmap_l1_analyze_window(&ws, pcm, N_SAMP, FS_BITS, SR, WIN_MS, 3000000, &w);
    free(pcm);

    EXPECT(w.valid);
    EXPECT(w.flatness > 0.4);
    EXPECT(w.bandwidth_hz > 500.0);
    return 0;
}

static int test_silence(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);

    dbmap_l1_window_t w;
    dbmap_l1_analyze_window(&ws, pcm, N_SAMP, FS_BITS, SR, WIN_MS, 4000000, &w);
    free(pcm);

    EXPECT(!w.valid);
    return 0;
}

static int test_below_energy_floor(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    fill_tone(pcm, N_SAMP, 1500.0, 1.0e-9);

    dbmap_l1_window_t w;
    dbmap_l1_analyze_window(&ws, pcm, N_SAMP, FS_BITS, SR, WIN_MS, 5000000, &w);
    free(pcm);

    EXPECT(!w.valid);
    return 0;
}

static int test_clipped(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    fill_tone(pcm, N_SAMP, 800.0, 1.0);
    /* Force a few samples to full-scale so peak_dbfs >= clip gate. */
    pcm[10] = (int32_t)FS - 1;
    pcm[11] = -(int32_t)FS + 1;

    dbmap_l1_window_t w;
    dbmap_l1_analyze_window(&ws, pcm, N_SAMP, FS_BITS, SR, WIN_MS, 6000000, &w);
    free(pcm);

    EXPECT(w.clipped);
    /* May still be spectrally valid; interval aggregation excludes clipped. */
    return 0;
}

static int test_insufficient_samples(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t pcm[512];
    memset(pcm, 0, sizeof(pcm));
    for (int i = 0; i < 512; i++) {
        pcm[i] = (int32_t)(0.1 * FS * sin(2.0 * M_PI * 1000.0 * i / SR));
    }

    dbmap_l1_window_t w;
    dbmap_l1_analyze_window(&ws, pcm, 512, FS_BITS, SR, WIN_MS, 7000000, &w);
    EXPECT(!w.valid);
    EXPECT(w.frame_count == 0);
    return 0;
}

static int test_interval_mean_stdev(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);

    dbmap_l1_interval_accum_t acc;
    dbmap_l1_interval_accum_init(&acc, SR, WIN_MS, 0, 5000000);

    fill_tone(pcm, N_SAMP, 1000.0, 0.2);
    dbmap_l1_window_t w0;
    dbmap_l1_analyze_window(&ws, pcm, N_SAMP, FS_BITS, SR, WIN_MS, 1000000, &w0);
    EXPECT(w0.valid);
    dbmap_l1_interval_accum_add(&acc, &w0);

    fill_tone(pcm, N_SAMP, 2000.0, 0.2);
    dbmap_l1_window_t w1;
    dbmap_l1_analyze_window(&ws, pcm, N_SAMP, FS_BITS, SR, WIN_MS, 2000000, &w1);
    EXPECT(w1.valid);
    dbmap_l1_interval_accum_add(&acc, &w1);

    /* Outside interval */
    dbmap_l1_window_t w_out = w1;
    w_out.window_end_mono_us = 6000000;
    dbmap_l1_interval_accum_add(&acc, &w_out);

    /* Clipped associated → invalid_window_count */
    w1.clipped = true;
    w1.window_end_mono_us = 3000000;
    dbmap_l1_interval_accum_add(&acc, &w1);

    dbmap_l1_interval_t iv;
    dbmap_l1_interval_accum_finalize(&acc, &iv);
    free(pcm);

    EXPECT(iv.window_count == 2);
    EXPECT(iv.invalid_window_count == 1);
    EXPECT(iv.has_centroid && iv.has_stdev);
    EXPECT(iv.centroid_mean_hz > 1200.0 && iv.centroid_mean_hz < 1800.0);
    EXPECT(iv.centroid_stdev_hz > 100.0);
    EXPECT(iv.fft_size == 1024);
    return 0;
}

static int test_empty_interval(void)
{
    dbmap_l1_interval_t iv;
    dbmap_l1_interval_set_empty(&iv, SR, WIN_MS, 0, 1000);
    EXPECT(iv.window_count == 0);
    EXPECT(!iv.has_centroid);
    EXPECT(iv.fft_size == DBMAP_L1_FFT_SIZE);
    return 0;
}

int main(void)
{
    if (test_single_tone()) return 1;
    if (test_two_tone()) return 1;
    if (test_white_noise()) return 1;
    if (test_silence()) return 1;
    if (test_below_energy_floor()) return 1;
    if (test_clipped()) return 1;
    if (test_insufficient_samples()) return 1;
    if (test_interval_mean_stdev()) return 1;
    if (test_empty_interval()) return 1;
    puts("ok");
    return 0;
}
