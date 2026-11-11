#pragma once

/**
 * Platform analysis-PCM handoff (double-buffered SPIRAM windows).
 *
 * Capture fills buffers; product analysis takes jobs via sp_mic_take_pcm_job.
 * No product acoustic types live here.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const int32_t *samples;
    size_t sample_count;
    int full_scale_bits;
    uint32_t window_ms;
    int64_t window_end_mono_us;
} sp_mic_pcm_job_t;

typedef void (*sp_mic_pcm_ready_hook_t)(uint32_t sample_rate_hz);

/** True after successful SPIRAM PCM double-buffer allocation. */
bool sp_mic_pcm_ready(void);

/**
 * Take the pending analysis PCM job and clear pending.
 * Returns false when L0/PCM capture is off or no window is pending.
 * out may be NULL to discard (still clears pending).
 */
bool sp_mic_take_pcm_job(sp_mic_pcm_job_t *out);

uint32_t sp_mic_pcm_cap_samples(void);
uint32_t sp_mic_pcm_bytes_total(void);
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
