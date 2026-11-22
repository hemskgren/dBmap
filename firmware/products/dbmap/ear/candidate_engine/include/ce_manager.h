/**
 * Candidate Manager — lifecycle owner for acoustic candidates.
 *
 * Storage stays on ce_engine_t (slots, next_id, cfg, diag) so the existing
 * promotion bridge can keep reading slots. Mutations go through this module.
 *
 * Association policy (Stage 1 identity — not a classifier):
 *
 * 1. NULL engine/signature, or an engine that is not started, is ignored.
 * 2. Signatures that fail ae_signature_usable_for_tracking (including
 *    AE_QUALITY_INVALID) are ignored. CE does not re-validate AE time or
 *    history; publish rejection stays in the Audio Engine.
 * 3. Zero, duplicate and rewound window sequence numbers are ignored.
 * 4. A forward sequence discontinuity or AE_QUALITY_GAP applies one
 *    transport coast step and increments gap_events once, even if both
 *    reasons apply. A usable arriving signature can still rematch.
 * 5. AE_QUALITY_GAP never matches or creates. quality_summary stays at the
 *    last matched signature. A discontinuous input uses the transport coast
 *    path only, so it cannot also spend the valid-window retention budget.
 * 6. Identity distance (CE-local, not ae_signature_distance):
 *      w_rms * |ΔRMS|  (low weight — RMS is not the primary identity cue)
 *    + w_band * Σ |Δ(band − RMS)|  (relative band shape)
 *    + spectral terms only when candidate.spectral_ema_valid && sig.spectral_valid
 *    + pair ΔRMS term only when candidate.pair_ema_valid && sig.pair_valid
 *    Missing/invalid spectral or pair features contribute nothing (not zeros).
 *    Pair ΔRMS is a stereo asymmetry cue only — not DoA/TDOA.
 * 7. Rematch: best in-use candidate with distance <= cfg.match_distance_max
 *    (default 8) is updated. If a second candidate is also <= match_max and
 *    (second − best) < cfg.match_ambiguity_delta (default 1.5), count
 *    match_ambiguous; still update only the best. One signature updates at
 *    most one candidate.
 * 8. Hysteresis: if best distance is in (match_max, match_max +
 *    match_hysteresis] (default hysteresis 2), neither update nor create
 *    (CE_INGEST_HYSTERESIS / match_hysteresis_hold). Prevents duplicate
 *    birth around the match boundary without blocking clearly distinct
 *    sources (distance > match_max + hysteresis → create path).
 * 9. No match beyond hysteresis: create NASCENT when quality is OK or
 *    DEGRADED and rms_dbfs >= cfg.min_rms_dbfs (default -80). That floor is
 *    an energy/eligibility gate only — not an identity or promotion threshold.
 * 10. NASCENT becomes ACTIVE when evidence_count >= cfg.n_enter (default 2,
 *     minimum 1) and quality is OK or DEGRADED.
 * 11. Transport coast: ACTIVE and PROMOTED move to COASTING (coast_windows = 1).
 *     COASTING increments; when coast_windows > cfg.t_coast (default 3,
 *     minimum 1) the slot expires. NASCENT expires on the first coast step.
 *     Expiry sets EXPIRED, in_use false. If promotion_entered, one
 *     CE_PROMOTION_EXIT is emitted and the flag is cleared.
 *     Valid-window retention uses a separate count (valid_miss_limit).
 *     A single missed hard match is tolerated before unsupported ACTIVE
 *     and PROMOTED candidates coast. Plausible ambiguous or hysteresis
 *     matches can defer coasting, but only a hard match resets the count
 *     and recovers a coasting candidate. NASCENT keeps its evidence state
 *     until it reaches n_enter or expires. Expiry is before allocation.
 * 12. Capacity is CE_MAX_CANDIDATES (8). A free slot is used first. Otherwise
 *     the NASCENT with the lowest evidence_count is evicted (equal counts
 *     keep the earlier slot). If none are NASCENT, the create is dropped.
 * 13. candidate_id is a monotonic device-local id starting at 1. It is stable
 *     until the slot is cleared and is not an Observation id.
 * 14. COASTING rematch restores PROMOTED when promotion_entered is set, and
 *     ACTIVE otherwise. That restoration does not emit another ENTER.
 * 15. Promotion and rejection are explicit (ce_promote / ce_reject). Creating,
 *     updating, or a module/workflow KEEP does not promote. Product promotion
 *     remains ear_promotion_policy.
 * 16. Engine stop (ce_manager_release_for_stop): remove live candidates without
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
    /** Near an existing candidate but outside rematch; create suppressed. */
    CE_INGEST_HYSTERESIS = 7,
} ce_ingest_action_t;

typedef struct {
    ce_ingest_action_t action;
    uint64_t candidate_id;
    bool seq_gap;
} ce_ingest_t;

ce_ingest_t ce_manager_ingest(ce_engine_t *eng, const ae_signature_t *sig);
bool ce_manager_promote(ce_engine_t *eng, uint64_t candidate_id);
bool ce_manager_unadmit_promotion(
    ce_engine_t *eng,
    uint64_t candidate_id,
    uint32_t promotion_gen
);
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
