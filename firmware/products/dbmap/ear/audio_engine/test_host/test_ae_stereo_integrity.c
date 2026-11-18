/**
 * Host tests: AE stereo integrity (unpack + pair path, swap-sensitive).
 *
 * Does not duplicate the full Platform stereo_unpack suite; focuses on the
 * AE pair/signature contract that consumes channel identity.
 *
 *   cc -std=c11 -Wall -Wextra -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     -I firmware/platform/audio/include \
 *     -I firmware/platform/mic/include \
 *     -I firmware/platform/node_common/include \
 *     firmware/platform/mic/i2s_stereo.c \
 *     firmware/platform/mic/audio_metrics.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_align.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_stereo.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/audio_engine/test_host/test_ae_stereo_integrity.c \
 *     -lm -o /tmp/test_ae_stereo_integrity && /tmp/test_ae_stereo_integrity
 */
#include "ae_align.h"
#include "ae_signature.h"
#include "ae_stereo.h"
#include "sp_audio_metrics.h"
#include "sp_i2s_stereo.h"

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
    int64_t t_us,
    float rms,
    float peak,
    bool samples
)
{
    ae_channel_snap_t s;
    memset(&s, 0, sizeof(s));
    s.present = true;
    s.window_seq = seq;
    s.measured_at_us = t_us;
    s.has_samples = samples;
    s.rms_dbfs = rms;
    s.peak_dbfs = peak;
    s.degraded = false;
    return s;
}

static void fill_l0(dbmap_l0_window_t *l0, float rms, int64_t t_us)
{
    memset(l0, 0, sizeof(*l0));
    l0->valid = true;
    l0->window_end_mono_us = t_us;
    l0->window_ms = 1000;
    l0->sample_count = 16000;
    l0->rms_dbfs = rms;
    l0->peak_dbfs = rms + 6.0f;
    l0->crest_db = 6.0f;
    l0->band_dbfs[0] = rms - 2.0f;
    l0->band_dbfs[1] = rms;
    l0->band_dbfs[2] = rms - 1.0f;
    l0->band_dbfs[3] = rms - 5.0f;
    l0->hl_valid = true;
    l0->hl_db = 1.0f;
}

/**
 * Contract under test: even slot → left/mic0, odd → right/mic1
 * (sp_i2s_stereo.h). Distinct payloads detect accidental L/R swap.
 */
static void test_unpack_feeds_distinct_lr(void)
{
    /* Positive 24-bit payloads (MSB clear) — same contract as test_stereo_unpack. */
    const uint32_t slots[4] = {
        0x11111100u, /* left  → 0x111111 */
        0x22222200u, /* right → 0x222222 */
        0x33333300u,
        0x44444400u,
    };
    int32_t left[2] = {0};
    int32_t right[2] = {0};
    uint32_t incomplete = 0;
    const size_t pairs = sp_i2s_unpack_stereo_slots(
        slots, 4, sp_inmp441_extract_sample, left, right, 2, &incomplete
    );
    EXPECT(pairs == 2);
    EXPECT(incomplete == 0);
    EXPECT(left[0] == 0x111111);
    EXPECT(right[0] == 0x222222);
    EXPECT(left[1] == 0x333333);
    EXPECT(right[1] == 0x444444);
    EXPECT(left[0] != right[0]);
    EXPECT(sp_i2s_stereo_channel_index(0) == 0);
    EXPECT(sp_i2s_stereo_channel_index(1) == 1);
    EXPECT(sp_i2s_mic_index_for_din_slot(0, 0) == 0);
    EXPECT(sp_i2s_mic_index_for_din_slot(0, 1) == 1);
}

/**
 * ae_signature_build: delta_rms_db = mic0_rms_dbfs - mic1_rms_dbfs.
 * Louder mic0 (less-negative dBFS) ⇒ positive delta.
 */
static void test_pair_stronger_mic0_and_swap(void)
{
    dbmap_l0_window_t l0;
    fill_l0(&l0, -40.0f, 1000000);

    ae_align_input_t in;
    memset(&in, 0, sizeof(in));
    in.pcm_window_seq = 10u;
    in.pcm_end_mono_us = 1000000;
    in.pcm_seq_valid = true;
    in.mic0 = snap(10u, 1000000, -40.0f, -34.0f, true);
    in.mic1 = snap(10u, 1000000, -52.0f, -46.0f, true);

    ae_align_result_t ares;
    ae_align_channels(&in, &ares);
    EXPECT(ares.pair_aligned);
    EXPECT(ares.pair.valid);
    EXPECT(ares.pair.mic_mask == 0x03u);

    ae_stereo_eval_t st =
        ae_stereo_eval(&ares, true, 0x03u, 16000, 16000);
    EXPECT(st.state == AE_STEREO_OK);

    ae_signature_t sig;
    EXPECT(ae_signature_build(10u, &l0, NULL, NULL, &ares.pair, false, &sig));
    EXPECT(sig.pair_valid);
    EXPECT(sig.mic_mask == 0x03u);
    /* Verified against ae_signature.c: mic0 - mic1. */
    EXPECT(fabsf(sig.delta_rms_db - 12.0f) < 0.01f);
    EXPECT(sig.delta_rms_db > 0.0f);

    /* Swap channel snaps: stronger energy now on mic1. */
    in.mic0 = snap(10u, 1000000, -52.0f, -46.0f, true);
    in.mic1 = snap(10u, 1000000, -40.0f, -34.0f, true);
    ae_align_channels(&in, &ares);
    EXPECT(ares.pair_aligned);
    EXPECT(ae_signature_build(10u, &l0, NULL, NULL, &ares.pair, false, &sig));
    EXPECT(fabsf(sig.delta_rms_db - (-12.0f)) < 0.01f);
    EXPECT(sig.delta_rms_db < 0.0f);
}

static void test_successive_windows_preserve_channel_identity(void)
{
    dbmap_l0_window_t l0;
    float prev_delta = 0.0f;

    for (uint32_t seq = 1u; seq <= 3u; seq++) {
        const int64_t t = (int64_t)seq * 1000000;
        fill_l0(&l0, -41.0f, t);

        ae_align_input_t in;
        memset(&in, 0, sizeof(in));
        in.pcm_window_seq = seq;
        in.pcm_end_mono_us = t;
        in.pcm_seq_valid = true;
        /* Stable asymmetry across windows. */
        in.mic0 = snap(seq, t, -38.0f, -32.0f, true);
        in.mic1 = snap(seq, t, -50.0f, -44.0f, true);

        ae_align_result_t ares;
        ae_align_channels(&in, &ares);
        EXPECT(ares.pair_aligned);

        ae_signature_t sig;
        EXPECT(ae_signature_build(seq, &l0, NULL, NULL, &ares.pair, false, &sig));
        EXPECT(sig.pair_valid);
        EXPECT(sig.mic_mask == 0x03u);
        EXPECT(sig.delta_rms_db > 0.0f);
        if (seq > 1u) {
            EXPECT(fabsf(sig.delta_rms_db - prev_delta) < 0.01f);
        }
        prev_delta = sig.delta_rms_db;
    }
}

static void test_mismatch_not_valid_silence_pair(void)
{
    ae_align_input_t in;
    memset(&in, 0, sizeof(in));
    in.pcm_window_seq = 20u;
    in.pcm_end_mono_us = 20000000;
    in.pcm_seq_valid = true;
    in.mic0 = snap(20u, 20000000, -40.0f, -34.0f, true);
    /* Same wall time would be rare; wrong seq must reject. */
    in.mic1 = snap(19u, 20000000, -120.0f, -120.0f, true);

    ae_align_result_t ares;
    ae_align_channels(&in, &ares);
    EXPECT(!ares.pair_aligned);
    EXPECT(!ares.pair.valid);
    EXPECT((ares.mismatch_flags & AE_ALIGN_MISMATCH_MIC1) != 0u);

    ae_stereo_eval_t st =
        ae_stereo_eval(&ares, true, 0x03u, 16000, 16000);
    EXPECT(st.state == AE_STEREO_MISALIGNED);

    dbmap_l0_window_t l0;
    fill_l0(&l0, -40.0f, 20000000);
    ae_signature_t sig;
    EXPECT(ae_signature_build(20u, &l0, NULL, NULL, NULL, false, &sig));
    EXPECT(!sig.pair_valid);
    EXPECT((sig.flags & AE_FLAG_PAIR_MISSING) != 0u);
    /* Must not fabricate a silence pair (delta would look like 80 dB). */
    EXPECT(fabsf(sig.delta_rms_db) < 0.01f);
}

int main(void)
{
    test_unpack_feeds_distinct_lr();
    test_pair_stronger_mic0_and_swap();
    test_successive_windows_preserve_channel_identity();
    test_mismatch_not_valid_silence_pair();
    if (g_failures != 0) {
        fprintf(stderr, "test_ae_stereo_integrity: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ae_stereo_integrity: ok\n");
    return 0;
}
