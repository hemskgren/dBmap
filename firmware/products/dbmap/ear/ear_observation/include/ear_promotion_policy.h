/**
 * EAR Promotion Policy — decides which CE candidates may be promoted.
 *
 * Owns reserve → bind → ce_promote (and release on promote failure).
 * Observation create/bind remains with ear_obs_manager (ENTER/commit).
 *
 * P0 eligibility: ACTIVE, quality OK/DEGRADED, no competing promoted lifecycle.
 * Ranking: evidence_count ↓, ema_rms_dbfs ↓, candidate_id ↑ (same as former bridge).
 * Does not use fixed RMS enter thresholds as promotion criteria.
 */
#pragma once

#include "ce_engine.h"
#include "ear_obs_manager.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t select_ok;
    uint32_t select_none;
    uint32_t reserve_ok;
    uint32_t reserve_full;
    uint32_t promote_ok;
    uint32_t promote_fail;
    uint32_t promote_skipped_busy;
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
 * Deterministic ACTIVE selection among candidates that have not yet entered
 * promotion. Other already-promoted candidates do not block (capacity-2 Obs).
 * Ranking: evidence_count ↓, ema_rms_dbfs ↓, candidate_id ↑.
 */
bool ear_promotion_select_primary(const ce_engine_t *ce, uint64_t *out_candidate_id);

/**
 * If a qualifying primary exists and capacity allows: reserve, bind, promote.
 * On ce_promote failure: release reservation. ENTER is committed via manager poll.
 */
void ear_promotion_policy_poll(ear_promotion_policy_t *pol, int64_t now_mono_us);

void ear_promotion_policy_get_diag(
    const ear_promotion_policy_t *pol,
    ear_promotion_policy_diag_t *out
);

#ifdef __cplusplus
}
#endif
