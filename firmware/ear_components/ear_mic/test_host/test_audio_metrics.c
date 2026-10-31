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
#include <string.h>

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

    /* EAR-001 rail: DMA slot 0x80000000 → extracted -2^23 (exact 24-bit FS).
     * Peak 0 dBFS is then genuine pre-shift MSB data, not invented by >> 8. */
    EXPECT(dbmap_inmp441_extract_sample(0x80000000u) == (int32_t)(-8388608));
    EXPECT(dbmap_inmp441_extract_sample(0x00000000u) == 0);
    /* Low-byte-only noise cannot become FS via >> 8. */
    EXPECT(dbmap_inmp441_extract_sample(0x000000FFu) == 0);
}

static void test_window_status_missing_mic(void)
{
    EXPECT(dbmap_audio_expected_samples(16000, 1000) == 16000);

    /* EAR-001/002 style: no samples. */
    EXPECT(strcmp(dbmap_audio_window_status(0, 16000, 0, 20, 1), "error") == 0);
    EXPECT(strcmp(dbmap_audio_window_status(0, 16000, 0, 0, 0), "error") == 0);

    /* EAR-003 style: one 256-frame buffer in a 1 s window + many stalls. */
    EXPECT(strcmp(dbmap_audio_window_status(256, 16000, 0, 100, 0), "error") == 0);

    /* Partial fill with stalls → degraded. */
    EXPECT(strcmp(dbmap_audio_window_status(9000, 16000, 0, 3, 0), "degraded") == 0);

    /* Quiet but well-filled window stays transport ok (silence ≠ I2S failure). */
    EXPECT(strcmp(dbmap_audio_window_status(16000, 16000, 0, 0, 0), "ok") == 0);
}

static void test_microphone_status_heuristic(void)
{
    EXPECT(strcmp(dbmap_audio_microphone_status("error", -120.0f), "unavailable") == 0);
    EXPECT(strcmp(dbmap_audio_microphone_status(NULL, -40.0f), "unavailable") == 0);
    EXPECT(
        strcmp(dbmap_audio_microphone_status("ok", -120.0f), "disconnected_suspected") == 0
    );
    EXPECT(
        strcmp(dbmap_audio_microphone_status("degraded", -120.0f), "disconnected_suspected")
        == 0
    );
    EXPECT(strcmp(dbmap_audio_microphone_status("ok", -42.5f), "signal_present") == 0);
}

static void test_classify_ear001_floating_din(void)
{
    /* Captured EAR-001 window with no physical mic. */
    char reason[48];
    dbmap_audio_mic_class_input_t in = {
        .transport_status = "ok",
        .sample_count = 16384,
        .rms_dbfs = -36.0f,
        .peak_dbfs = 0.0f,
        .diag = {
            .raw_min = -8388608,
            .raw_max = 2097151,
            .zero_sample_count = 15468,
            .full_scale_sample_count = 1,
            .unique_sample_count = 38,
            .consecutive_identical_samples = 624,
            .unique_capped = 0,
        },
    };
    EXPECT(strcmp(dbmap_audio_classify_microphone(&in, reason, sizeof(reason)), "noise_suspected") == 0);
    EXPECT(strcmp(reason, "mostly_zero_with_rail_hits") == 0);
}

static void test_classify_digital_silence_peers(void)
{
    char reason[48];
    dbmap_audio_mic_class_input_t in = {
        .transport_status = "ok",
        .sample_count = 16384,
        .rms_dbfs = -120.0f,
        .peak_dbfs = -120.0f,
        .diag = {
            .raw_min = 0,
            .raw_max = 0,
            .zero_sample_count = 16384,
            .full_scale_sample_count = 0,
            .unique_sample_count = 1,
            .consecutive_identical_samples = 16384,
            .unique_capped = 0,
        },
    };
    EXPECT(
        strcmp(dbmap_audio_classify_microphone(&in, reason, sizeof(reason)), "disconnected_suspected")
        == 0
    );
    EXPECT(strcmp(reason, "digital_silence") == 0);
}

static void test_classify_ambient_like_quiet_mic(void)
{
    char reason[48];
    /* Quiet room: diverse near-zero values, no rails, not exact-zero dominated. */
    dbmap_audio_mic_class_input_t in = {
        .transport_status = "ok",
        .sample_count = 16000,
        .rms_dbfs = -55.0f,
        .peak_dbfs = -28.0f,
        .diag = {
            .raw_min = -50000,
            .raw_max = 40000,
            .zero_sample_count = 200,
            .full_scale_sample_count = 0,
            .unique_sample_count = 800,
            .consecutive_identical_samples = 12,
            .unique_capped = 0,
        },
    };
    EXPECT(strcmp(dbmap_audio_classify_microphone(&in, reason, sizeof(reason)), "signal_present") == 0);
    EXPECT(strcmp(reason, "ambient_like_energy") == 0);
}

int main(void)
{
    test_silence_and_empty();
    test_full_scale_peak();
    test_minus_six_db();
    test_clip_count();
    test_inmp441_extract();
    test_window_status_missing_mic();
    test_microphone_status_heuristic();
    test_classify_ear001_floating_din();
    test_classify_digital_silence_peers();
    test_classify_ambient_like_quiet_mic();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
