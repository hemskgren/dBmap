/**
 * Host unit tests for I2S stereo unpack / channel identity helpers.
 *
 * Build/run from repo root:
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_mic/include \
 *     firmware/ear_components/ear_mic/i2s_stereo.c \
 *     firmware/ear_components/ear_mic/audio_metrics.c \
 *     firmware/ear_components/ear_mic/test_host/test_stereo_unpack.c \
 *     -lm -o /tmp/test_dbmap_stereo_unpack && /tmp/test_dbmap_stereo_unpack
 *
 * These tests encode the ESP-IDF ESP32-S3 STD RX documented buffer order
 * (WS-low then WS-high → left then right). They do not prove physical mic wiring.
 */
#include "dbmap_audio_metrics.h"
#include "dbmap_i2s_stereo.h"

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

static void test_words_per_frame(void)
{
    EXPECT(dbmap_i2s_slot_words_per_frame(1) == 1);
    EXPECT(dbmap_i2s_slot_words_per_frame(2) == 2);
    EXPECT(dbmap_i2s_slot_words_per_frame(0) == 1);
}

static void test_channel_index_deterministic(void)
{
    /* Software contract: even=left/mic0, odd=right/mic1. */
    EXPECT(dbmap_i2s_stereo_channel_index(0) == 0);
    EXPECT(dbmap_i2s_stereo_channel_index(1) == 1);
    EXPECT(dbmap_i2s_stereo_channel_index(2) == 0);
    EXPECT(dbmap_i2s_stereo_channel_index(3) == 1);
}

static void test_incomplete_frames(void)
{
    EXPECT(dbmap_i2s_stereo_complete_pairs(0) == 0);
    EXPECT(dbmap_i2s_stereo_complete_pairs(1) == 0);
    EXPECT(dbmap_i2s_stereo_complete_pairs(2) == 1);
    EXPECT(dbmap_i2s_stereo_complete_pairs(3) == 1);
    EXPECT(dbmap_i2s_stereo_complete_pairs(4) == 2);
    EXPECT(dbmap_i2s_stereo_incomplete_trailing(0) == 0);
    EXPECT(dbmap_i2s_stereo_incomplete_trailing(1) == 1);
    EXPECT(dbmap_i2s_stereo_incomplete_trailing(2) == 0);
    EXPECT(dbmap_i2s_stereo_incomplete_trailing(5) == 1);
}

static void test_unpack_channel_ordering(void)
{
    /*
     * Synthesize IDF RX stereo buffer: [L0, R0, L1, R1].
     * Distinct left-justified 24-bit payloads so mapping is unambiguous.
     */
    const uint32_t slots[5] = {
        0x11111100u, /* left 0 → PCM 0x111111 */
        0x22222200u, /* right 0 → PCM 0x222222 */
        0x33333300u, /* left 1 */
        0x44444400u, /* right 1 */
        0x55555500u, /* incomplete trailing — must be ignored */
    };
    int32_t left[4] = {0};
    int32_t right[4] = {0};
    uint32_t incomplete = 0;
    const size_t pairs = dbmap_i2s_unpack_stereo_slots(
        slots,
        5,
        dbmap_inmp441_extract_sample,
        left,
        right,
        4,
        &incomplete
    );
    EXPECT(pairs == 2);
    EXPECT(incomplete == 1);
    EXPECT(left[0] == 0x111111);
    EXPECT(right[0] == 0x222222);
    EXPECT(left[1] == 0x333333);
    EXPECT(right[1] == 0x444444);
    /* Trailing incomplete must not write pair index 2. */
    EXPECT(left[2] == 0);
    EXPECT(right[2] == 0);
}

static void test_unpack_sample_counts_even(void)
{
    uint32_t slots[4] = {0x01000000u, 0x02000000u, 0x03000000u, 0x04000000u};
    int32_t left[2];
    int32_t right[2];
    uint32_t incomplete = 99;
    const size_t pairs = dbmap_i2s_unpack_stereo_slots(
        slots, 4, dbmap_inmp441_extract_sample, left, right, 2, &incomplete
    );
    EXPECT(pairs == 2);
    EXPECT(incomplete == 0);
    EXPECT(left[0] == 0x010000);
    EXPECT(right[0] == 0x020000);
    EXPECT(left[1] == 0x030000);
    EXPECT(right[1] == 0x040000);
}

static void test_unpack_respects_max_pairs(void)
{
    uint32_t slots[6] = {
        0x01000000u, 0x02000000u, 0x03000000u, 0x04000000u, 0x05000000u, 0x06000000u
    };
    int32_t left[1] = {0};
    int32_t right[1] = {0};
    uint32_t incomplete = 0;
    const size_t pairs = dbmap_i2s_unpack_stereo_slots(
        slots, 6, dbmap_inmp441_extract_sample, left, right, 1, &incomplete
    );
    EXPECT(pairs == 1);
    EXPECT(incomplete == 0);
    EXPECT(left[0] == 0x010000);
    EXPECT(right[0] == 0x020000);
}

static void test_channel_presence_vs_health(void)
{
    /* Delivering samples is not enough to call a channel healthy. */
    EXPECT(
        dbmap_ear_channel_presence(0, 0, 0) == DBMAP_EAR_CH_NO_SAMPLES
    );
    EXPECT(
        strcmp(dbmap_ear_channel_presence_str(DBMAP_EAR_CH_NO_SAMPLES), "no_samples") == 0
    );

    EXPECT(
        dbmap_ear_channel_presence(16000, 16000, 0) == DBMAP_EAR_CH_SILENT
    );
    EXPECT(
        strcmp(dbmap_ear_channel_presence_str(DBMAP_EAR_CH_SILENT), "silent") == 0
    );

    EXPECT(
        dbmap_ear_channel_presence(16000, 100, 12) == DBMAP_EAR_CH_CLIPPING
    );
    EXPECT(
        strcmp(dbmap_ear_channel_presence_str(DBMAP_EAR_CH_CLIPPING), "clipping") == 0
    );

    EXPECT(
        dbmap_ear_channel_presence(16000, 100, 0) == DBMAP_EAR_CH_ACTIVE
    );
    EXPECT(
        strcmp(dbmap_ear_channel_presence_str(DBMAP_EAR_CH_ACTIVE), "active") == 0
    );
}

static void test_expected_samples_unchanged_per_channel(void)
{
    /* Each channel still expects rate*window samples (not 2×). */
    EXPECT(dbmap_audio_expected_samples(16000, 1000) == 16000);
}

int main(void)
{
    test_words_per_frame();
    test_channel_index_deterministic();
    test_incomplete_frames();
    test_unpack_channel_ordering();
    test_unpack_sample_counts_even();
    test_unpack_respects_max_pairs();
    test_channel_presence_vs_health();
    test_expected_samples_unchanged_per_channel();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
