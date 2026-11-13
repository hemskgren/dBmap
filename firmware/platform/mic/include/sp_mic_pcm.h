#pragma once

/**
 * Platform analysis-PCM handoff (double-buffered SPIRAM windows).
 *
 * Capture fills buffers; product analysis takes jobs via sp_mic_take_pcm_job /
 * sp_mic_take_window_job. No product acoustic types live here.
 *
 * M5: Mic0 (left) and Mic1 (right) PCM may both be retained when live_mic_count
 * ≥ 2. samples / sample_count remain Mic0 aliases for Observation/L0 mono.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Mic0 / left PCM (primary). Alias of samples_ch0. */
    const int32_t *samples;
    size_t sample_count;

    /** Explicit per-channel PCM (M5). NULL / 0 when channel not retained. */
    const int32_t *samples_ch0;
    size_t sample_count_ch0;
    const int32_t *samples_ch1;
    size_t sample_count_ch1;
    /** bit0=ch0 present, bit1=ch1 present. Never invent silent channels. */
    uint8_t pcm_channel_mask;

    int full_scale_bits;
    uint32_t window_ms;
    int64_t window_end_mono_us;
    /**
     * Same measure_window sequence as sp_mic_get_channel/stream for this
     * capture. 0 means unset/legacy. Consumers must not assume get_channel()
     * returns this window without verifying window_seq and measured_at_us.
     */
    uint32_t window_seq;
} sp_mic_pcm_job_t;

typedef void (*sp_mic_pcm_ready_hook_t)(uint32_t sample_rate_hz);

/** True after successful SPIRAM PCM double-buffer allocation. */
bool sp_mic_pcm_ready(void);

/**
 * Take the pending analysis PCM job and clear pending.
 * Returns false when L0/PCM capture is off or no window is pending.
 * out may be NULL to discard (still clears pending).
 *
 * For stereo pair metrics + dual PCM, prefer sp_mic_take_window_job().
 */
bool sp_mic_take_pcm_job(sp_mic_pcm_job_t *out);

uint32_t sp_mic_pcm_cap_samples(void);
/** Total SPIRAM bytes for all PCM banks × retained channels. */
uint32_t sp_mic_pcm_bytes_total(void);
/** 1 (mono) or 2 (stereo retention allocated). */
uint8_t sp_mic_pcm_retained_channels(void);
uint32_t sp_mic_pcm_pending_overwrite(void);
uint32_t sp_mic_pcm_free_heap_after_init(void);
uint32_t sp_mic_pcm_free_spiram_after_init(void);

/**
 * Install a one-shot-style ready hook. Called once after successful SPIRAM PCM
 * alloc during sp_mic_start (when CONFIG_DBMAP_EAR_MIC_L0). Set before
 * mic start so product analysis can init workspaces at the same point.
 */
void sp_mic_set_pcm_ready_hook(sp_mic_pcm_ready_hook_t hook);

#ifdef __cplusplus
}
#endif
