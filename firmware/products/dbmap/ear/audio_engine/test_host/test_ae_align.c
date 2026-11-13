/**
 * Host tests: PCM ↔ channel association and pair metrics.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     firmware/products/dbmap/ear/audio_engine/ae_align.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/audio_engine/test_host/test_ae_align.c \
 *     -lm -o /tmp/test_ae_align && /tmp/test_ae_align
 */
#include "ae_align.h"
#include "ae_signature.h"

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

static ae_channel_snap_t snap(
    uint32_t seq,
    int64_t t,
    float rms,
    float peak,
    bool samples,
    bool degraded
)
{
    ae_channel_snap_t s;
    memset(&s, 0, sizeof(s));
    s.present = true;
    s.window_seq = seq;
    s.measured_at_us = t;
    s.has_samples = samples;
    s.rms_dbfs = rms;
    s.peak_dbfs = peak;
    s.degraded = degraded;
    return s;
}

static void test_pair_aligned(void)
{
    ae_align_input_t in;
    memset(&in, 0, sizeof(in));
    in.pcm_window_seq = 10;
    in.pcm_end_mono_us = 5000000;
    in.pcm_seq_valid = true;
    in.mic0 = snap(10, 5000000, -40.0f, -34.0f, true, false);
    in.mic1 = snap(10, 5000000, -46.0f, -40.0f, true, false);
    ae_align_result_t out;
    ae_align_channels(&in, &out);
    EXPECT(out.mic0_aligned);
    EXPECT(out.mic1_aligned);
    EXPECT(out.pair_aligned);
    EXPECT(out.pair.valid);
    EXPECT(fabsf(out.pair.mic0_rms_dbfs - (-40.0f)) < 0.01f);
    EXPECT(fabsf(out.pair.mic1_rms_dbfs - (-46.0f)) < 0.01f);
    EXPECT(!out.pair.stereo_degraded);
    EXPECT(out.mismatch_flags == 0u);
}

static void test_stale_channel_rejected(void)
{
    ae_align_input_t in;
    memset(&in, 0, sizeof(in));
    in.pcm_window_seq = 11;
    in.pcm_end_mono_us = 6000000;
    in.pcm_seq_valid = true;
    in.mic0 = snap(10, 5000000, -40.0f, -34.0f, true, false); /* stale */
    in.mic1 = snap(11, 6000000, -46.0f, -40.0f, true, false);
    ae_align_result_t out;
    ae_align_channels(&in, &out);
    EXPECT(!out.mic0_aligned);
    EXPECT(out.mic1_aligned);
    EXPECT(!out.pair_aligned);
    EXPECT(!out.pair.valid);
    EXPECT((out.mismatch_flags & AE_ALIGN_MISMATCH_MIC0) != 0u);
}

static void test_seq_mismatch_rejected(void)
{
    ae_align_input_t in;
    memset(&in, 0, sizeof(in));
    in.pcm_window_seq = 12;
    in.pcm_end_mono_us = 7000000;
    in.pcm_seq_valid = true;
    /* Same timestamp but wrong seq must not combine. */
    in.mic0 = snap(11, 7000000, -40.0f, -34.0f, true, false);
    in.mic1 = snap(12, 7000000, -46.0f, -40.0f, true, false);
    ae_align_result_t out;
    ae_align_channels(&in, &out);
    EXPECT(!out.mic0_aligned);
    EXPECT(out.mic1_aligned);
    EXPECT(!out.pair_aligned);
}

static void test_missing_mic1_not_zero(void)
{
    ae_align_input_t in;
    memset(&in, 0, sizeof(in));
    in.pcm_window_seq = 13;
    in.pcm_end_mono_us = 8000000;
    in.pcm_seq_valid = true;
    in.mic0 = snap(13, 8000000, -40.0f, -34.0f, true, false);
    /* mic1 absent */
    ae_align_result_t out;
    ae_align_channels(&in, &out);
    EXPECT(out.mic0_aligned);
    EXPECT(!out.mic1_aligned);
    EXPECT(!out.pair_aligned);
    EXPECT(!out.pair.valid);
}

static void test_no_samples_not_pair(void)
{
    ae_align_input_t in;
    memset(&in, 0, sizeof(in));
    in.pcm_window_seq = 14;
    in.pcm_end_mono_us = 9000000;
    in.pcm_seq_valid = true;
    in.mic0 = snap(14, 9000000, -40.0f, -34.0f, true, false);
    in.mic1 = snap(14, 9000000, -120.0f, -120.0f, false, true); /* no samples */
    ae_align_result_t out;
    ae_align_channels(&in, &out);
    EXPECT(out.mic0_aligned);
    EXPECT(out.mic1_aligned);
    EXPECT(!out.pair_aligned);
}

static void test_l1_present_l2_missing_ok(void)
{
    dbmap_l0_window_t l0;
    memset(&l0, 0, sizeof(l0));
    l0.valid = true;
    l0.window_end_mono_us = 1000;
    l0.window_ms = 1000;
    l0.rms_dbfs = -40.0f;
    l0.peak_dbfs = -34.0f;
    l0.crest_db = 6.0f;
    l0.band_dbfs[0] = -43.0f;
    l0.band_dbfs[1] = -40.0f;
    l0.band_dbfs[2] = -41.0f;
    l0.band_dbfs[3] = -46.0f;
    l0.hl_valid = true;

    dbmap_l1_window_t l1;
    memset(&l1, 0, sizeof(l1));
    l1.valid = true;
    l1.centroid_hz = 900.0f;
    l1.bandwidth_hz = 300.0f;
    l1.dominant_hz = 880.0f;
    l1.flatness = 0.25f;

    ae_pair_metrics_t pair = {
        .valid = true,
        .mic0_rms_dbfs = -40.0f,
        .mic1_rms_dbfs = -45.0f,
        .mic0_peak_dbfs = -34.0f,
        .mic1_peak_dbfs = -39.0f,
        .mic_mask = 0x03u,
        .stereo_degraded = false,
    };

    ae_signature_t sig;
    EXPECT(ae_signature_build(20u, &l0, &l1, NULL, &pair, false, &sig));
    EXPECT(sig.quality == AE_QUALITY_OK);
    EXPECT((sig.flags & AE_FLAG_L2_MISSING) != 0u);
    EXPECT(sig.pair_valid);
}

int main(void)
{
    test_pair_aligned();
    test_stale_channel_rejected();
    test_seq_mismatch_rejected();
    test_missing_mic1_not_zero();
    test_no_samples_not_pair();
    test_l1_present_l2_missing_ok();
    if (g_failures != 0) {
        fprintf(stderr, "test_ae_align: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ae_align: ok\n");
    return 0;
}
