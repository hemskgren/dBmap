#pragma once

/**
 * Sensor Platform multi-microphone audio analysis.
 *
 * Runs every build-enabled DSP layer (SP_AUDIO_L0/L1/L2) on every active
 * microphone channel. Channel identity and shared window_seq /
 * window_end_mono_us are preserved on each result.
 *
 * Resource strategy: one L0 filter bank and one L1 workspace (~22.5 KiB) are
 * reused serially across channels. Do not allocate per-mic L1 workspaces.
 *
 * Host-testable; no FreeRTOS / product Observation policy.
 */

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#include "sp_acoustic_l0.h"
#include "sp_acoustic_l1.h"
#include "sp_acoustic_l2.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SP_AUDIO_ANALYZE_MAX_CHANNELS
#define SP_AUDIO_ANALYZE_MAX_CHANNELS 4u
#endif

#define SP_AUDIO_ANALYZE_OK 0
#define SP_AUDIO_ANALYZE_ERR_INVALID_ARG (-1)
#define SP_AUDIO_ANALYZE_ERR_NOT_READY (-2)

/**
 * Per-window multi-channel PCM view for analysis (Platform contract, not a
 * product type). Mirrors the retained fields of sp_mic_pcm_job_t without
 * requiring the mic component as a link dependency of every DSP consumer.
 */
typedef struct {
    const int32_t *samples_ch[SP_AUDIO_ANALYZE_MAX_CHANNELS];
    size_t sample_count_ch[SP_AUDIO_ANALYZE_MAX_CHANNELS];
    /** Bit i set when samples_ch[i] is present. Never invent silent channels. */
    uint8_t pcm_channel_mask;
    int full_scale_bits;
    uint32_t window_ms;
    int64_t window_end_mono_us;
    uint32_t window_seq;
} sp_audio_analyze_input_t;

/** Per-channel analysis result for one acquisition window. */
typedef struct {
    uint8_t channel_id;
    uint32_t window_seq;
    int64_t window_end_mono_us;
    /** False when PCM for this active channel was missing/empty. */
    bool input_valid;
#if defined(CONFIG_SP_AUDIO_L0) && CONFIG_SP_AUDIO_L0
    sp_acoustic_l0_window_t l0;
#endif
#if defined(CONFIG_SP_AUDIO_L1) && CONFIG_SP_AUDIO_L1
    sp_acoustic_l1_window_t l1;
#endif
#if defined(CONFIG_SP_AUDIO_L2) && CONFIG_SP_AUDIO_L2
    sp_acoustic_l2_window_t l2;
#endif
} sp_audio_channel_result_t;

/** Analysis results for all active channels of one window. */
typedef struct {
    uint32_t window_seq;
    int64_t window_end_mono_us;
    uint8_t active_mic_count;
    /** Bit i set when channels[i] was filled (active index only). */
    uint8_t result_mask;
    sp_audio_channel_result_t channels[SP_AUDIO_ANALYZE_MAX_CHANNELS];
} sp_audio_window_result_t;

/**
 * Serial analysis context. Owns one L0 bank + one L1 workspace reused across
 * channels. Init once after PCM sample-rate is known.
 */
typedef struct {
    uint32_t sample_rate_hz;
    bool ready;
#if defined(CONFIG_SP_AUDIO_L0) && CONFIG_SP_AUDIO_L0
    sp_acoustic_l0_filter_bank_t l0_bank;
#endif
#if defined(CONFIG_SP_AUDIO_L1) && CONFIG_SP_AUDIO_L1
    sp_acoustic_l1_workspace_t l1_ws;
#endif
} sp_audio_analyze_ctx_t;

/**
 * Prepare filter bank / L1 workspace for sample_rate_hz.
 * Safe to call again when rate changes (re-inits workspaces).
 */
void sp_audio_analyze_ctx_init(sp_audio_analyze_ctx_t *ctx, uint32_t sample_rate_hz);

/**
 * Analyze every active microphone with every build-enabled DSP layer.
 *
 * active_mic_count is the runtime live count (NVS mic_count → acquire).
 * Only indexes [0, active_mic_count) are processed. Inactive channels produce
 * no results (result_mask bits stay clear). Missing PCM for an active channel
 * yields input_valid=false without attributing another channel's samples.
 *
 * Returns SP_AUDIO_ANALYZE_OK on success (including partial invalid inputs).
 */
int sp_audio_analyze_channels(
    sp_audio_analyze_ctx_t *ctx,
    const sp_audio_analyze_input_t *in,
    uint8_t active_mic_count,
    sp_audio_window_result_t *out
);

/** True when layer Ln is compiled into this build. */
bool sp_audio_analyze_layer_enabled(unsigned layer /* 0,1,2 */);

#ifdef __cplusplus
}
#endif
