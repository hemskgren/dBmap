/**
 * Pending analysis-window slot. Caller holds the microphone lock.
 * PCM banks, the fill index, and s_lock stay in ear_mic.c.
 * The overwrite counter survives sp_mic_window_slot_reset_pending().
 */
#pragma once

#include "sp_mic_pcm.h"
#include "sp_mic_window_job.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool pending;
    int buf_idx;
    size_t sample_count_ch[SP_MIC_COUNT_MAX];
    uint8_t pcm_channel_mask;
    uint32_t window_ms;
    int64_t window_end_mono_us;
    int full_scale_bits;
    uint32_t window_seq;
    uint8_t live_mic_count;
    sp_mic_window_view_t views[SP_MIC_COUNT_MAX];
} sp_mic_window_slot_pending_t;

/** Clear the pending record. Does not change the overwrite counter. */
void sp_mic_window_slot_reset_pending(void);

uint32_t sp_mic_window_slot_overwrite_count(void);
void sp_mic_window_slot_note_projection(bool legacy, bool projected);
void sp_mic_window_slot_get_diag(sp_mic_window_diag_t *out);

/**
 * Replace the pending job. A replacement of a job that is still pending
 * increments the overwrite counter. Forces pending true.
 */
void sp_mic_window_slot_publish(const sp_mic_window_slot_pending_t *in);

/**
 * Copy the pending job and clear pending. Returns false when none is pending.
 * Does not look at PCM banks.
 */
bool sp_mic_window_slot_take(sp_mic_window_slot_pending_t *out);

/**
 * Project a taken snapshot onto caller-owned banks.
 * Returns false when buf_idx is unusable or Mic0's bank is NULL.
 * out NULL discards a usable bank and returns true.
 * banks is int32_t * [SP_MIC_COUNT_MAX][2], indexed [channel][buf].
 */
bool sp_mic_window_slot_project_pcm(
    const sp_mic_window_slot_pending_t *snap,
    int32_t *(*banks)[2],
    sp_mic_pcm_job_t *out
);

/**
 * PCM projection plus bundled channel views.
 * sample_rate_hz is the rate captured with the take.
 * A failed channel fill leaves the mask at 0 and still returns true,
 * matching the previous take path.
 */
bool sp_mic_window_slot_project_window(
    const sp_mic_window_slot_pending_t *snap,
    int32_t *(*banks)[2],
    uint32_t sample_rate_hz,
    sp_mic_window_job_t *out
);

#ifdef __cplusplus
}
#endif
