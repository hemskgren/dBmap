/**
 * EAR Promotion Policy — product-relevance gate for CE → Observation promote.
 *
 * Owns reserve → bind → ce_promote (and release on promote failure).
 * Observation create/bind remains with ear_obs_manager (ENTER/commit).
 *
 * P2: deny-by-default. Ambient ACTIVE energy alone never authorizes promote.
 * First experimental allow signal: AS-001 helicopter-like classification
 * annotation (provisional profile match — not verified aircraft detection),
 * fresh vs candidate gap_events. Successful decisions stamp a reservation
 * relevance ticket carried to ENTER (not reconstructed at OM).
 * Non-relevant candidates are soft-skipped (no ce_reject).
 *
 * Ranking among approved candidates: evidence_count ↓, ema_rms_dbfs ↓,
 * candidate_id ↑. Does not use fixed RMS enter thresholds as promotion criteria.
 */
#pragma once

#include "ce_engine.h"
#include "ear_obs_manager.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EAR_PROMO_DECISION_NONE = 0,
    EAR_PROMO_DECISION_PROMOTE = 1,
    EAR_PROMO_DECISION_HOLD_IRRELEVANT = 2,
    EAR_PROMO_DECISION_HOLD_NO_CLASS = 3,
    EAR_PROMO_DECISION_HOLD_BUSY = 4,
    EAR_PROMO_DECISION_HOLD_RESERVE_FULL = 5,
    EAR_PROMO_DECISION_PROMOTE_FAIL = 6,
} ear_promo_decision_t;

typedef struct {
    uint32_t select_ok;
    uint32_t select_none;
    uint32_t hold_irrelevant;
    uint32_t hold_no_class;
    uint32_t reserve_ok;
    uint32_t reserve_full;
    uint32_t promote_ok;
    uint32_t promote_fail;
    uint32_t promote_skipped_busy;
    /** Last evaluated / acted-on candidate (0 if none). */
    uint64_t last_candidate_id;
    ear_promo_decision_t last_decision;
    uint16_t last_profile_id;
    float last_match_distance;
} ear_promotion_policy_diag_t;

typedef struct {
    ce_engine_t *ce;
    ear_obs_manager_t *om;
    ear_promotion_policy_diag_t diag;
} ear_promotion_policy_t;

void ear_promotion_policy_init(
    ear_promotion_policy_t *pol,
    ce_engine_t *ce,
    ear_obs_manager_t *om
);

/**
 * True when CE classification annotates an experimental AS-001 match.
 * Does not promote; used by policy and OM snapshot stamping.
 */
bool ear_promotion_candidate_product_relevant(
    const ce_engine_t *ce,
    uint64_t candidate_id,
    uint16_t *out_profile_id,
    float *out_distance
);

/**
 * Deterministic selection among product-relevant ACTIVE candidates that have
 * not yet entered promotion. Ranking: evidence_count ↓, ema_rms_dbfs ↓,
 * candidate_id ↑.
 */
bool ear_promotion_select_primary(const ce_engine_t *ce, uint64_t *out_candidate_id);

/**
 * If a relevance-approved primary exists and capacity allows: reserve, bind,
 * promote. On ce_promote failure: release reservation. Non-relevant candidates
 * are soft-skipped. ENTER is committed via manager poll.
 */
void ear_promotion_policy_poll(ear_promotion_policy_t *pol, int64_t now_mono_us);

void ear_promotion_policy_get_diag(
    const ear_promotion_policy_t *pol,
    ear_promotion_policy_diag_t *out
);

#ifdef __cplusplus
}
#endif
