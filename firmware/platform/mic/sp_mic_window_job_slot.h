/**
 * Pending analysis-window slot. Caller holds the microphone lock.
 * PCM storage and s_lock stay in ear_mic.c.
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
#if CONFIG_DBMAP_EAR_AE_DIAG
    uint32_t bank_generation;
#endif
    size_t sample_count_ch[SP_MIC_COUNT_MAX];
    uint8_t pcm_channel_mask;
    uint32_t window_ms;
    int64_t window_end_mono_us;
    int64_t collection_start_mono_us;
    uint32_t incomplete_stereo_frames;
    int full_scale_bits;
    uint32_t window_seq;
    uint32_t software_missing_frames;
    uint16_t carried_frames;
    uint8_t span_end;
    uint8_t live_mic_count;
    sp_mic_window_view_t views[SP_MIC_COUNT_MAX];
    /* Set on take, not on publication. */
    uint32_t pending_overwrites_at_take;
    uint64_t lease_token;
} sp_mic_window_slot_pending_t;

/** Retire pending only. Outstanding leases and fills survive. */
void sp_mic_window_slot_reset_pending(void);

uint32_t sp_mic_window_slot_overwrite_count(void);
void sp_mic_window_slot_get_diag(sp_mic_window_diag_t *out);

/** Reserve a bank before writing. May retire unread pending; -1 means drop PCM. */
int sp_mic_window_slot_begin_fill(void);
/** Retire a fill that never published. */
void sp_mic_window_slot_cancel_fill(int bank);
/** Commit a FILLING bank and retire any prior unread pending bank. */
bool sp_mic_window_slot_publish(const sp_mic_window_slot_pending_t *in);

/** Take and pin pending, or retire it directly when discard is true. */
bool sp_mic_window_slot_take(sp_mic_window_slot_pending_t *out, bool discard);
/** Count projection outcome and release a failed projection's lease. */
void sp_mic_window_slot_note_projection(uint64_t token, bool legacy, bool projected);
bool sp_mic_window_slot_release(uint64_t token);
void sp_mic_window_slot_note_legacy_generation_mismatch(void);

/**
 * Project a taken snapshot onto caller-owned banks.
 * Returns false when buf_idx is unusable or a required channel bank is NULL.
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
