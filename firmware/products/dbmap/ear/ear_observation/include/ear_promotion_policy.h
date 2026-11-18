/**
 * EAR Promotion Policy — product-relevance gate for CE → Observation promote.
 *
 * Owns reserve → bind → ce_promote (and release on promote failure).
 * Observation create/bind remains with ear_obs_manager (ENTER/commit).
 *
 * P2: deny-by-default. Ambient ACTIVE energy alone never authorizes promote.
 * Allow signals:
 *   - AS-001 helicopter-like classification annotation (provisional)
 *   - Optional SE public-alert siren evidence (ear_siren_se_public) when
 *     attached via ear_promotion_policy_set_siren — ALARM streak only; does
 *     not emit FARAN_OVER/VMA leaves.
 * Successful decisions stamp a reservation relevance ticket carried to ENTER
 * (not reconstructed at OM). Non-relevant candidates are soft-skipped
 * (no ce_reject).
 *
 * Same-signature guard: at most one concurrent open/pending Observation per
 * relevance_profile_id (AS-001 vs AS-002 are different signatures). Does not
 * impose a global one-Observation-per-EAR limit. Future spatial/DOA keys may
 * refine identity further; this gate uses the promote profile stamp only.
 *
 * Ranking among approved candidates: evidence_count ↓, ema_rms_dbfs ↓,
 * candidate_id ↑. Does not use fixed RMS enter thresholds as promotion criteria.
 */
#pragma once

#include "ce_engine.h"
#include "ear_obs_manager.h"
#include "ear_siren_se_public.h"

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
    /** Open/pending Obs already exists for this relevance_profile_id. */
    EAR_PROMO_DECISION_HOLD_SIGNATURE_OPEN = 7,
} ear_promo_decision_t;

typedef struct {
    uint32_t select_ok;
    uint32_t select_none;
    uint32_t hold_irrelevant;
    uint32_t hold_no_class;
    uint32_t hold_signature_open;
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
    /**
     * Optional standalone detector for host tests.
     * Production uses as002_from_engine instead (published Signature Engine).
     */
    const ear_siren_se_public_t *siren;
    /** When true, AS-002 promote eligibility uses sig_engine published state. */
    bool as002_from_engine;
    ear_promotion_policy_diag_t diag;
} ear_promotion_policy_t;

void ear_promotion_policy_init(
    ear_promotion_policy_t *pol,
    ce_engine_t *ce,
    ear_obs_manager_t *om
);

/** Attach optional siren evidence source (may be NULL to detach). */
void ear_promotion_policy_set_siren(
    ear_promotion_policy_t *pol,
    const ear_siren_se_public_t *siren
);

/**
 * Use Signature Engine published AS-002 state for promote eligibility
 * (production). Clears any standalone siren pointer.
 */
void ear_promotion_policy_use_sig_engine_as002(
    ear_promotion_policy_t *pol,
    bool enable
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
 * True when candidate may promote via AS-001 **or** attached siren ALARM
 * evidence. Soft-skip ambient-only candidates when neither applies.
 * Does not apply the same-signature open guard (caller checks that).
 */
bool ear_promotion_candidate_promote_relevant(
    const ear_promotion_policy_t *pol,
    uint64_t candidate_id,
    uint16_t *out_profile_id,
    float *out_distance
);

/**
 * Deterministic selection among product-relevant ACTIVE candidates that have
 * not yet entered promotion and whose relevance_profile_id is not already
 * represented by an open/pending Observation. Ranking: evidence_count ↓,
 * ema_rms_dbfs ↓, candidate_id ↑.
 */
bool ear_promotion_select_primary(
    const ear_promotion_policy_t *pol,
    uint64_t *out_candidate_id
);

/**
 * If a relevance-approved primary exists and capacity allows: reserve, bind,
 * promote. On ce_promote failure: release reservation. Non-relevant candidates
 * are soft-skipped. Same-signature open/pending → HOLD_SIGNATURE_OPEN.
 * ENTER is committed via manager poll.
 */
void ear_promotion_policy_poll(ear_promotion_policy_t *pol, int64_t now_mono_us);

void ear_promotion_policy_get_diag(
    const ear_promotion_policy_t *pol,
    ear_promotion_policy_diag_t *out
);

#ifdef __cplusplus
}
#endif
