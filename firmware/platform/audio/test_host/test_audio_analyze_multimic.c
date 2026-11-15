/**
 * Host tests: Platform multi-mic analysis (every active mic × enabled layers).
 *
 *   cc -std=c11 -Wall -Wextra -O2 -lm \
 *     -DCONFIG_SP_AUDIO_L0=1 -DCONFIG_SP_AUDIO_L1=1 -DCONFIG_SP_AUDIO_L2=1 \
 *     -I firmware/platform/audio/include \
 *     firmware/platform/audio/sp_acoustic_l0.c \
 *     firmware/platform/audio/sp_acoustic_l1.c \
 *     firmware/platform/audio/sp_acoustic_l2.c \
 *     firmware/platform/audio/sp_audio_analyze.c \
 *     firmware/platform/audio/test_host/test_audio_analyze_multimic.c \
 *     -o /tmp/test_audio_analyze_multimic && /tmp/test_audio_analyze_multimic
 */
#include "sp_audio_analyze.h"

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

enum { k_rate = 16000, k_window_ms = 64, k_fs_bits = 24, k_n = 1024 };

static void fill_tone(int32_t *dst, size_t n, double freq_hz, double amp)
{
    const double fs = (double)(1 << (k_fs_bits - 1));
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i / (double)k_rate;
        dst[i] = (int32_t)(amp * fs * sin(2.0 * 3.14159265358979323846 * freq_hz * t));
    }
}

static void fill_silence(int32_t *dst, size_t n)
{
    memset(dst, 0, n * sizeof(int32_t));
}

static void make_input(
    sp_audio_analyze_input_t *in,
    uint8_t mask,
    uint32_t seq,
    int64_t t_end,
    int32_t *bufs[SP_AUDIO_ANALYZE_MAX_CHANNELS],
    size_t n
)
{
    memset(in, 0, sizeof(*in));
    in->pcm_channel_mask = mask;
    in->full_scale_bits = k_fs_bits;
    in->window_ms = k_window_ms;
    in->window_end_mono_us = t_end;
    in->window_seq = seq;
    for (uint8_t ch = 0; ch < SP_AUDIO_ANALYZE_MAX_CHANNELS; ++ch) {
        if ((mask & (uint8_t)(1u << ch)) != 0u && bufs[ch] != NULL) {
            in->samples_ch[ch] = bufs[ch];
            in->sample_count_ch[ch] = n;
        }
    }
}

static void test_layer_flags(void)
{
    EXPECT(sp_audio_analyze_layer_enabled(0));
    EXPECT(sp_audio_analyze_layer_enabled(1));
    EXPECT(sp_audio_analyze_layer_enabled(2));
    EXPECT(!sp_audio_analyze_layer_enabled(3));
}

/**
 * NVS mic_count drives active_mic_count. Primary verification targets are
 * mic_count=1 and mic_count=2; 3–4 remain covered for Platform capability.
 */
static void test_mic_counts_and_isolation(void)
{
    sp_audio_analyze_ctx_t ctx;
    sp_audio_analyze_ctx_init(&ctx, k_rate);
    EXPECT(ctx.ready);

    int32_t ch0[k_n];
    int32_t ch1[k_n];
    int32_t ch2[k_n];
    int32_t ch3[k_n];
    fill_tone(ch0, k_n, 440.0, 0.2);
    fill_tone(ch1, k_n, 880.0, 0.25);
    fill_tone(ch2, k_n, 220.0, 0.15);
    fill_tone(ch3, k_n, 1320.0, 0.3);
    int32_t *bufs[SP_AUDIO_ANALYZE_MAX_CHANNELS] = { ch0, ch1, ch2, ch3 };

    for (uint8_t active = 1; active <= 4; ++active) {
        const uint8_t mask = (uint8_t)((1u << active) - 1u);
        sp_audio_analyze_input_t in;
        make_input(&in, mask, 100u + active, 5000000 + (int64_t)active, bufs, k_n);

        sp_audio_window_result_t out;
        EXPECT(sp_audio_analyze_channels(&ctx, &in, active, &out) == SP_AUDIO_ANALYZE_OK);
        EXPECT(out.window_seq == 100u + active);
        EXPECT(out.window_end_mono_us == 5000000 + (int64_t)active);
        EXPECT(out.active_mic_count == active);
        EXPECT(out.result_mask == mask);

        for (uint8_t ch = 0; ch < active; ++ch) {
            EXPECT(out.channels[ch].channel_id == ch);
            EXPECT(out.channels[ch].window_seq == out.window_seq);
            EXPECT(out.channels[ch].window_end_mono_us == out.window_end_mono_us);
            EXPECT(out.channels[ch].input_valid);
            EXPECT(out.channels[ch].l0.valid);
            EXPECT(out.channels[ch].l1.valid);
            /* Fused L2 path stamps the shared window time on each channel. */
            EXPECT(out.channels[ch].l2.window_end_mono_us == out.window_end_mono_us);
            EXPECT(out.channels[ch].l2.window_ms == k_window_ms);
        }
        for (uint8_t ch = active; ch < SP_AUDIO_ANALYZE_MAX_CHANNELS; ++ch) {
            EXPECT((out.result_mask & (uint8_t)(1u << ch)) == 0u);
            /* Inactive slots must stay zeroed (no leaked results). */
            EXPECT(!out.channels[ch].input_valid);
            EXPECT(!out.channels[ch].l0.valid);
            EXPECT(!out.channels[ch].l1.valid);
            EXPECT(out.channels[ch].l2.window_end_mono_us == 0);
        }

        /* Distinct tones must not leak across channels (RMS / dominant differ). */
        if (active >= 2u) {
            EXPECT(fabsf(out.channels[0].l0.rms_dbfs - out.channels[1].l0.rms_dbfs) > 0.01f
                   || fabsf(out.channels[0].l1.dominant_hz - out.channels[1].l1.dominant_hz)
                       > 50.0f);
        }
    }
}

static void test_missing_channel_explicit(void)
{
    sp_audio_analyze_ctx_t ctx;
    sp_audio_analyze_ctx_init(&ctx, k_rate);

    int32_t ch0[k_n];
    int32_t ch2[k_n];
    fill_tone(ch0, k_n, 440.0, 0.2);
    fill_tone(ch2, k_n, 220.0, 0.2);
    /* active=3 but bit1 missing — channel 1 must be input_valid=false. */
    int32_t *bufs[SP_AUDIO_ANALYZE_MAX_CHANNELS] = { ch0, NULL, ch2, NULL };
    sp_audio_analyze_input_t in;
    make_input(&in, 0x05u /* bits 0 and 2 */, 42u, 9000000, bufs, k_n);

    sp_audio_window_result_t out;
    EXPECT(sp_audio_analyze_channels(&ctx, &in, 3u, &out) == SP_AUDIO_ANALYZE_OK);
    EXPECT(out.result_mask == 0x07u);
    EXPECT(out.channels[0].input_valid);
    EXPECT(out.channels[0].l0.valid);
    EXPECT(!out.channels[1].input_valid);
    EXPECT(!out.channels[1].l0.valid);
    EXPECT(out.channels[2].input_valid);
    EXPECT(out.channels[2].l0.valid);
    EXPECT(out.channels[0].window_seq == 42u);
    EXPECT(out.channels[1].window_seq == 42u);
    EXPECT(out.channels[2].window_seq == 42u);
}

static void test_invalid_args(void)
{
    sp_audio_analyze_ctx_t ctx;
    sp_audio_analyze_ctx_init(&ctx, k_rate);
    sp_audio_analyze_input_t in;
    memset(&in, 0, sizeof(in));
    sp_audio_window_result_t out;
    EXPECT(sp_audio_analyze_channels(NULL, &in, 1u, &out) == SP_AUDIO_ANALYZE_ERR_INVALID_ARG);
    EXPECT(sp_audio_analyze_channels(&ctx, NULL, 1u, &out) == SP_AUDIO_ANALYZE_ERR_INVALID_ARG);
    EXPECT(sp_audio_analyze_channels(&ctx, &in, 1u, NULL) == SP_AUDIO_ANALYZE_ERR_INVALID_ARG);
    EXPECT(sp_audio_analyze_channels(&ctx, &in, 0u, &out) == SP_AUDIO_ANALYZE_ERR_INVALID_ARG);
    EXPECT(sp_audio_analyze_channels(&ctx, &in, 5u, &out) == SP_AUDIO_ANALYZE_ERR_INVALID_ARG);

    sp_audio_analyze_ctx_t unready;
    memset(&unready, 0, sizeof(unready));
    EXPECT(
        sp_audio_analyze_channels(&unready, &in, 1u, &out) == SP_AUDIO_ANALYZE_ERR_NOT_READY
    );
}

static void test_silence_channel_no_leak(void)
{
    sp_audio_analyze_ctx_t ctx;
    sp_audio_analyze_ctx_init(&ctx, k_rate);
    int32_t loud[k_n];
    int32_t quiet[k_n];
    fill_tone(loud, k_n, 1000.0, 0.4);
    fill_silence(quiet, k_n);
    int32_t *bufs[SP_AUDIO_ANALYZE_MAX_CHANNELS] = { loud, quiet, NULL, NULL };
    sp_audio_analyze_input_t in;
    make_input(&in, 0x03u, 7u, 1000, bufs, k_n);
    sp_audio_window_result_t out;
    EXPECT(sp_audio_analyze_channels(&ctx, &in, 2u, &out) == SP_AUDIO_ANALYZE_OK);
    EXPECT(out.channels[0].l0.rms_dbfs > out.channels[1].l0.rms_dbfs + 20.0f);
    EXPECT(out.channels[0].channel_id == 0u);
    EXPECT(out.channels[1].channel_id == 1u);
}

int main(void)
{
    test_layer_flags();
    test_mic_counts_and_isolation();
    test_missing_channel_explicit();
    test_invalid_args();
    test_silence_channel_no_leak();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
