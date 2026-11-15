#pragma once

/**
 * Platform analysis-PCM handoff (double-buffered SPIRAM windows).
 *
 * Capture fills buffers; product analysis takes jobs via sp_mic_take_pcm_job /
 * sp_mic_take_window_job. No product acoustic types live here.
 *
 * When CONFIG_SP_MIC_PCM_RETAIN is enabled, PCM is retained for every live
 * microphone (NVS mic_count → live acquire count, 1..SP_MIC_COUNT_MAX).
 * samples / sample_count remain Mic0 aliases for Observation / primary paths.
 */

#include "sp_mic_count.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Mic0 PCM (primary). Alias of samples_ch[0] when bit0 is set. */
    const int32_t *samples;
    size_t sample_count;

    /**
     * Per-channel PCM. samples_ch[i] / sample_count_ch[i] are valid only when
     * (pcm_channel_mask & (1u << i)) != 0. Never invent silent channels.
     */
    const int32_t *samples_ch[SP_MIC_COUNT_MAX];
    size_t sample_count_ch[SP_MIC_COUNT_MAX];
    /** Bit i set when channel i PCM is present in this job. */
    uint8_t pcm_channel_mask;
    /** Live mic count at capture (1..SP_MIC_COUNT_MAX); inactive channels omitted. */
    uint8_t active_mic_count;

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
 * Prefer sp_mic_take_window_job() when channel snapshots are also required.
 */
bool sp_mic_take_pcm_job(sp_mic_pcm_job_t *out);

uint32_t sp_mic_pcm_cap_samples(void);
/** Total SPIRAM bytes for all PCM banks × retained channels. */
uint32_t sp_mic_pcm_bytes_total(void);
/** Number of channels allocated for retention (equals live count at start). */
uint8_t sp_mic_pcm_retained_channels(void);
uint32_t sp_mic_pcm_pending_overwrite(void);
uint32_t sp_mic_pcm_free_heap_after_init(void);
uint32_t sp_mic_pcm_free_spiram_after_init(void);

/**
 * Install a one-shot-style ready hook. Called once after successful SPIRAM PCM
 * alloc during sp_mic_start (when CONFIG_SP_MIC_PCM_RETAIN). Set before
 * mic start so product analysis can init workspaces at the same point.
 */
void sp_mic_set_pcm_ready_hook(sp_mic_pcm_ready_hook_t hook);

#ifdef __cplusplus
}
#endif
