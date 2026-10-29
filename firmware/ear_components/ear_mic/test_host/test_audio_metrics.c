/**
 * Host unit tests for Ear audio metric helpers (no ESP-IDF required).
 *
 * Build/run from repo root:
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_mic/include \
 *     firmware/ear_components/ear_mic/audio_metrics.c \
 *     firmware/ear_components/ear_mic/test_host/test_audio_metrics.c \
 *     -lm -o /tmp/test_dbmap_audio_metrics && /tmp/test_dbmap_audio_metrics
 */
#include "dbmap_audio_metrics.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define EXPECT_NEAR(actual, expected, tol)                                     \
    do {                                                                       \
        double _a = (double)(actual);                                          \
        double _e = (double)(expected);                                        \
        if (fabs(_a - _e) > (tol)) {                                           \
            fprintf(                                                           \
                stderr,                                                        \
                "FAIL %s:%d: %s=%g expected %g ±%g\n",                         \
                __FILE__,                                                      \
                __LINE__,                                                      \
                #actual,                                                       \
                _a,                                                            \
                _e,                                                            \
                (double)(tol)                                                  \
            );                                                                 \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void test_silence_and_empty(void)
{
    EXPECT_NEAR(dbmap_audio_rms_dbfs(NULL, 0, 24), -120.0, 0.01);
    EXPECT_NEAR(dbmap_audio_peak_dbfs(NULL, 0, 24), -120.0, 0.01);
    EXPECT(dbmap_audio_count_clipped(NULL, 0, 24) == 0);

    int32_t zeros[8] = {0};
    EXPECT_NEAR(dbmap_audio_rms_dbfs(zeros, 8, 24), -120.0, 0.01);
    EXPECT_NEAR(dbmap_audio_peak_dbfs(zeros, 8, 24), -120.0, 0.01);
}

static void test_full_scale_peak(void)
{
    /* 24-bit signed full scale magnitude is 2^23. */
    int32_t full = 1 << 23;
    int32_t samples[4] = {full, -full, full / 2, 0};
    EXPECT_NEAR(dbmap_audio_peak_dbfs(samples, 4, 24), 0.0, 0.05);
    EXPECT_NEAR(dbmap_audio_peak_dbfs(samples, 3, 24), 0.0, 0.05);
    int32_t one = full;
    EXPECT_NEAR(dbmap_audio_rms_dbfs(&one, 1, 24), 0.0, 0.05);
}

static void test_minus_six_db(void)
{
    int32_t half = 1 << 22; /* half of 2^23 → about -6.02 dBFS */
    int32_t samples[2] = {half, -half};
    EXPECT_NEAR(dbmap_audio_peak_dbfs(samples, 2, 24), -6.02, 0.05);
    EXPECT_NEAR(dbmap_audio_rms_dbfs(samples, 2, 24), -6.02, 0.05);
}

static void test_clip_count(void)
{
    int32_t fs = 1 << 23;
    int32_t samples[4] = {fs, (int32_t)(fs * 0.5), (int32_t)(fs * 0.995), 0};
    EXPECT(dbmap_audio_count_clipped(samples, 4, 24) == 2);
}

static void test_inmp441_extract(void)
{
    /* Left-justified 24-bit 0x123456 in top bits → 0x123456 after >> 8. */
    uint32_t word = 0x12345600u;
    EXPECT(dbmap_inmp441_extract_sample(word) == 0x123456);

    /* Sign extension for negative 24-bit values. */
    uint32_t neg = 0x80000000u;
    EXPECT(dbmap_inmp441_extract_sample(neg) < 0);
}

int main(void)
{
    test_silence_and_empty();
    test_full_scale_peak();
    test_minus_six_db();
    test_clip_count();
    test_inmp441_extract();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
