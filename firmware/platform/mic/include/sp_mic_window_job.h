/**
 * Coherent analysis-window handoff: PCM job + channel snapshots for the
 * same measure_window (same window_seq / measured_at).
 *
 * Prefer sp_mic_take_window_job() over take_pcm_job() + get_channel() so
 * publication ordering of s_latest cannot drop a valid stereo pair.
 */
#pragma once

#include "sp_mic_channel_api.h"
#include "sp_mic_count.h"
#include "sp_mic_pcm.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    sp_mic_pcm_job_t pcm;
    uint8_t live_mic_count;
    /** Bit i set when channels[i] was filled for this window (live index). */
    uint8_t channel_mask;
    sp_mic_channel_t channels[SP_MIC_COUNT_MAX];
} sp_mic_window_job_t;

/**
 * Host-testable: fill job.channels[] from window views that belong to job.pcm.
 * Sets channel_mask bits only for indices with measured_at_us > 0.
 * Does not invent zero-signal channels for missing views.
 */
int sp_mic_window_job_set_channels_from_views(
    sp_mic_window_job_t *job,
    uint8_t live_mic_count,
    uint32_t sample_rate_hz,
    const sp_mic_window_view_t *views
);

/**
 * True when every masked channel matches job.pcm.window_seq and
 * window_end_mono_us. Empty mask is coherent (PCM-only / mono path).
 */
bool sp_mic_window_job_channels_coherent(const sp_mic_window_job_t *job);

/**
 * Take pending PCM + matching channel snapshots atomically and clear pending.
 * Returns false when L0/PCM is off or no window is pending.
 * out may be NULL to discard.
 */
bool sp_mic_take_window_job(sp_mic_window_job_t *out);

typedef enum {
    SP_MIC_WINDOW_EMPTY,
    SP_MIC_WINDOW_PROJECTED,
    SP_MIC_WINDOW_PROJECTION_FAILED,
} sp_mic_window_take_result_t;

typedef struct {
    uint32_t published;
    uint32_t overwrites;
    uint32_t reset_discards;
    uint32_t legacy_takes;
    uint32_t projection_failed;
    bool pending;
    bool saturated;
} sp_mic_window_diag_t;

sp_mic_window_take_result_t sp_mic_take_window_job_result(sp_mic_window_job_t *out);
void sp_mic_get_window_diag(sp_mic_window_diag_t *out);


#ifdef __cplusplus
}
#endif
