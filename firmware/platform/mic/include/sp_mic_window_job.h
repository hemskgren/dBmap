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

typedef enum {
    SP_MIC_SPAN_COMPLETE,
    SP_MIC_SPAN_STALLED,
    SP_MIC_SPAN_DEADLINE,
} sp_mic_span_end_t;

typedef struct {
    sp_mic_pcm_job_t pcm;
    /** Software collection start, in the same monotonic clock as pcm.window_end_mono_us. */
    int64_t collection_start_mono_us;
    uint32_t nominal_sample_rate_hz;
    /** Incomplete trailing stereo frames discarded during this collection, summed across DINs. */
    uint32_t incomplete_stereo_frames;
    /** Known ordinal skips at/before consumed groups since the prior group (saturating). */
    uint32_t software_missing_frames;
    /** Initial complete groups consumed from prior reads (not a hardware timestamp). */
    uint16_t carried_frames;
    /** sp_mic_span_end_t, stored compactly. */
    uint8_t span_end;
    uint8_t live_mic_count;
    /** Bit i set when channels[i] was filled for this window (live index). */
    uint8_t channel_mask;
    sp_mic_channel_t channels[SP_MIC_COUNT_MAX];
} sp_mic_window_job_t;

/**
 * Derive the cap discard for a channel with both a captured view and retained PCM.
 * Returns false if that comparison is unavailable; leaves out untouched.
 * Transport faults are reported separately in the channel view and the window's
 * incomplete stereo count. Absence of those faults does not verify continuity.
 */
bool sp_mic_window_job_cap_discarded(
    const sp_mic_window_job_t *job, uint8_t channel, uint32_t *out
);

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
 * out may be NULL to discard. A non-NULL successful take acquires a lease;
 * release pcm.lease_token after the last PCM access.
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
    uint32_t pending_retired;
    uint32_t no_bank_drops;
    uint32_t unpublished_fills;
    uint32_t invalid_releases;
    uint32_t ownership_violations;
    uint32_t legacy_generation_mismatches;
    bool pending;
    bool saturated;
} sp_mic_window_diag_t;

sp_mic_window_take_result_t sp_mic_take_window_job_result(sp_mic_window_job_t *out);
void sp_mic_get_window_diag(sp_mic_window_diag_t *out);


#ifdef __cplusplus
}
#endif
