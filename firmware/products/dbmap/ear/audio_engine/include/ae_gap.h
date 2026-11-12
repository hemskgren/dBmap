/**
 * Pure window-gap evaluation for AE intake (host-testable).
 *
 * Distinguishes SP pending-overwrite deltas from window_seq holes.
 * Either condition marks the next published signature with AE_QUALITY_GAP.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool seq_gap;
    uint32_t overwrite_delta;
    bool gap_before;
} ae_window_gap_t;

/**
 * Evaluate discontinuity for one taken window.
 *
 * @param last_published_seq  AE last published seq (0 if none yet).
 * @param job_window_seq      Current job window_seq.
 * @param overwrite_counter_now  Current sp_mic_pcm_pending_overwrite().
 * @param inout_last_overwrite_seen  In/out baseline; updated to now.
 *        uint32 wrap: delta = now - *seen (modular).
 *
 * Seq gap: last != 0 && job_seq > last + 1 (unchanged rule).
 * Overwrite: delta > 0 (including first window after AE start baseline).
 * gap_before: seq_gap || overwrite_delta > 0 (single GAP flag).
 */
ae_window_gap_t ae_window_gap_eval(
    uint32_t last_published_seq,
    uint32_t job_window_seq,
    uint32_t overwrite_counter_now,
    uint32_t *inout_last_overwrite_seen
);

#ifdef __cplusplus
}
#endif
