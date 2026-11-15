#if !defined(ESP_PLATFORM) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#include "sp_audio_analyze.h"

#include <string.h>

void sp_audio_analyze_ctx_init(sp_audio_analyze_ctx_t *ctx, uint32_t sample_rate_hz)
{
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->sample_rate_hz = sample_rate_hz;
#if defined(CONFIG_SP_AUDIO_L0) && CONFIG_SP_AUDIO_L0
    if (sample_rate_hz > 0u) {
        sp_acoustic_l0_filter_bank_init(&ctx->l0_bank, sample_rate_hz);
        ctx->ready = true;
    }
#if defined(CONFIG_SP_AUDIO_L1) && CONFIG_SP_AUDIO_L1
    sp_acoustic_l1_workspace_init(&ctx->l1_ws);
#endif
#else
    (void)sample_rate_hz;
#endif
}

bool sp_audio_analyze_layer_enabled(unsigned layer)
{
    switch (layer) {
#if defined(CONFIG_SP_AUDIO_L0) && CONFIG_SP_AUDIO_L0
    case 0:
        return true;
#endif
#if defined(CONFIG_SP_AUDIO_L1) && CONFIG_SP_AUDIO_L1
    case 1:
        return true;
#endif
#if defined(CONFIG_SP_AUDIO_L2) && CONFIG_SP_AUDIO_L2
    case 2:
        return true;
#endif
    default:
        return false;
    }
}

int sp_audio_analyze_channels(
    sp_audio_analyze_ctx_t *ctx,
    const sp_audio_analyze_input_t *in,
    uint8_t active_mic_count,
    sp_audio_window_result_t *out
)
{
#if !defined(CONFIG_SP_AUDIO_L0) || !CONFIG_SP_AUDIO_L0
    (void)ctx;
    (void)in;
    (void)active_mic_count;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return SP_AUDIO_ANALYZE_ERR_NOT_READY;
#else
    if (ctx == NULL || in == NULL || out == NULL) {
        return SP_AUDIO_ANALYZE_ERR_INVALID_ARG;
    }
    if (!ctx->ready || ctx->sample_rate_hz == 0u) {
        return SP_AUDIO_ANALYZE_ERR_NOT_READY;
    }
    if (active_mic_count < 1u || active_mic_count > SP_AUDIO_ANALYZE_MAX_CHANNELS) {
        return SP_AUDIO_ANALYZE_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->window_seq = in->window_seq;
    out->window_end_mono_us = in->window_end_mono_us;
    out->active_mic_count = active_mic_count;

    for (uint8_t ch = 0; ch < active_mic_count; ++ch) {
        sp_audio_channel_result_t *r = &out->channels[ch];
        r->channel_id = ch;
        r->window_seq = in->window_seq;
        r->window_end_mono_us = in->window_end_mono_us;

        const bool have_pcm = ((in->pcm_channel_mask & (uint8_t)(1u << ch)) != 0u)
            && in->samples_ch[ch] != NULL && in->sample_count_ch[ch] > 0u;
        r->input_valid = have_pcm;
        out->result_mask = (uint8_t)(out->result_mask | (uint8_t)(1u << ch));

        if (!have_pcm) {
            /* Explicit invalid: do not copy or invent samples from another channel. */
            continue;
        }

        const int32_t *pcm = in->samples_ch[ch];
        const size_t n = in->sample_count_ch[ch];
        const int fs_bits = in->full_scale_bits;
        const uint32_t window_ms = in->window_ms;
        const int64_t t_end = in->window_end_mono_us;

        sp_acoustic_l0_analyze_window(
            &ctx->l0_bank, pcm, n, fs_bits, window_ms, t_end, &r->l0
        );

#if defined(CONFIG_SP_AUDIO_L1) && CONFIG_SP_AUDIO_L1
#if defined(CONFIG_SP_AUDIO_L2) && CONFIG_SP_AUDIO_L2
        /* Serial reuse of the single L1 workspace across channels. */
        sp_acoustic_l2_analyze_window(
            &ctx->l1_ws,
            pcm,
            n,
            fs_bits,
            ctx->sample_rate_hz,
            window_ms,
            t_end,
            &r->l1,
            &r->l2
        );
#else
        sp_acoustic_l1_analyze_window(
            &ctx->l1_ws,
            pcm,
            n,
            fs_bits,
            ctx->sample_rate_hz,
            window_ms,
            t_end,
            &r->l1
        );
#endif
#endif
    }

    return SP_AUDIO_ANALYZE_OK;
#endif
}
