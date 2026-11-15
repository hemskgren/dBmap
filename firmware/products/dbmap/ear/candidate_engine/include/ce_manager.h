/**
 * Candidate Manager — lifecycle owner for acoustic candidates.
 *
 * Storage stays on ce_engine_t (slots, next_id, cfg, diag) so the existing
 * promotion bridge can keep reading slots. Mutations go through this module.
 *
 * Association policy (preserved from the 0.8.11 Candidate Engine, not a
 * classifier):
 *
 * 1. NULL engine/signature, or an engine that is not started, is ignored.
 * 2. Signatures that fail ae_signature_usable_for_tracking (including
 *    AE_QUALITY_INVALID) are ignored. CE does not re-validate AE time or
 *    history; publish rejection stays in the Audio Engine.
 * 3. window_seq != 0 && window_seq <= last processed seq is a duplicate or
 *    rewind and is ignored. The AE timestamp is not replaced.
 * 4. If window_seq > last+1 and last != 0, one coast step is applied first
 *    (missed windows are not continuous evidence). The arrived signature is
 *    then processed. gap_events increments on each coast step.
 * 5. AE_QUALITY_GAP applies a coast step and returns. It does not match or
 *    create. quality_summary is left at the last matched signature so a
 *    coasting candidate does not lose the quality the bridge already uses
 *    for ACTIVE selection; the gap itself is gap_events plus COASTING.
 * 6. Otherwise the best in-use candidate with fingerprint distance <=
 *    cfg.match_distance_max (default 8) is updated. Distance is
 *    ae_signature_distance between the signature and an EMA proxy (RMS +
 *    band EMA only; spectral and pair fields zeroed). EMA alpha is 0.35.
 *    Ties keep the later slot (distance <= best). One signature updates at
 *    most one candidate.
 * 7. No match: create NASCENT when quality is OK or DEGRADED and
 *    rms_dbfs >= cfg.min_rms_dbfs (default -80). Otherwise do not create.
 * 8. NASCENT becomes ACTIVE when evidence_count >= cfg.n_enter (default 2,
 *    minimum 1) and quality is OK or DEGRADED.
 * 9. Coast: ACTIVE and PROMOTED move to COASTING (coast_windows = 1).
 *    COASTING increments; when coast_windows > cfg.t_coast (default 3,
 *    minimum 1) the slot expires. NASCENT expires on the first coast step.
 *    Expiry sets EXPIRED, in_use false. If promotion_entered, one
 *    CE_PROMOTION_EXIT is emitted and the flag is cleared.
 * 10. Capacity is CE_MAX_CANDIDATES (8). A free slot is used first. Otherwise
 *     the NASCENT with the lowest evidence_count is evicted (equal counts
 *     keep the earlier slot). If none are NASCENT, the create is dropped.
 * 11. candidate_id is a monotonic device-local id starting at 1. It is stable
 *     until the slot is cleared and is not an Observation id.
 * 12. COASTING rematch restores PROMOTED when promotion_entered is set, and
 *     ACTIVE otherwise. That restoration does not emit another ENTER.
 * 13. Promotion and rejection are explicit (ce_promote / ce_reject), including
 *     when a workflow decision asks for them. Creating, updating, or a module
 *     result does not promote.
 * 14. Engine stop (ce_manager_release_for_stop): remove live candidates without
 *     treating them as coast expiry. Emit one CE_PROMOTION_EXIT for every
 *     promotion_entered lifecycle so the Bridge can clear association. Do not
 *     open or end Observations. After release, slots are not in_use and cannot
 *     be promoted.
 */
#pragma once

#include "ce_engine.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CE_INGEST_IGNORED = 0,
    CE_INGEST_UNUSABLE = 1,
    CE_INGEST_DUPLICATE = 2,
    CE_INGEST_GAP = 3,
    CE_INGEST_UPDATED = 4,
    CE_INGEST_CREATED = 5,
    CE_INGEST_DROPPED = 6,
} ce_ingest_action_t;

typedef struct {
    ce_ingest_action_t action;
    uint64_t candidate_id;
    bool seq_gap;
} ce_ingest_t;

ce_ingest_t ce_manager_ingest(ce_engine_t *eng, const ae_signature_t *sig);
bool ce_manager_promote(ce_engine_t *eng, uint64_t candidate_id);
bool ce_manager_reject(ce_engine_t *eng, uint64_t candidate_id);

/**
 * Safe engine-stop release. Emits EXIT for promotion_entered slots, then clears
 * every in_use slot. Does not increment expired/rejected. Observation open/close
 * remains outside CE (Bridge EXIT only clears association).
 */
void ce_manager_release_for_stop(ce_engine_t *eng);

#ifdef __cplusplus
}
#endif
