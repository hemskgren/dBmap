#include "ear_promotion_policy.h"

#include "as_profile.h"

#include <string.h>

void ear_promotion_policy_init(
    ear_promotion_policy_t *pol,
    ce_engine_t *ce,
    ear_obs_manager_t *om
)
{
    if (pol == NULL) {
        return;
    }
    memset(pol, 0, sizeof(*pol));
    pol->ce = ce;
    pol->om = om;
}

bool ear_promotion_candidate_product_relevant(
    const ce_engine_t *ce,
    uint64_t candidate_id,
    uint16_t *out_profile_id,
    float *out_distance
)
{
    if (out_profile_id != NULL) {
        *out_profile_id = AS_PROFILE_ID_NONE;
    }
    if (out_distance != NULL) {
        *out_distance = 0.0f;
    }
    if (ce == NULL || candidate_id == 0u) {
        return false;
    }

    ce_classification_t cls;
    if (!ce_get_classification(ce, candidate_id, &cls) || !cls.valid) {
        return false;
    }
    if (!cls.matched
        || cls.profile_id != AS_PROFILE_ID_AS001
        || cls.acoustic_class != AS_CLASS_HELICOPTER_LIKE) {
        return false;
    }
    /* Freshness: annotation must post-date any GAP on this candidate. */
    ae_candidate_t cand;
    if (!ce_find_candidate(ce, candidate_id, &cand)
        || cls.gap_events_at_match != cand.gap_events) {
        return false;
    }
    if (out_profile_id != NULL) {
        *out_profile_id = cls.profile_id;
    }
    if (out_distance != NULL) {
        *out_distance = cls.distance;
    }
    return true;
}

bool ear_promotion_select_primary(const ce_engine_t *ce, uint64_t *out_candidate_id)
{
    if (ce == NULL || out_candidate_id == NULL || !ce->started) {
        return false;
    }

    /*
     * P2 deny-by-default: only product-relevant ACTIVE candidates compete.
     * Other already-promoted candidates do not block (capacity-2 Obs).
     */
    bool have = false;
    uint64_t best_id = 0u;
    uint32_t best_evidence = 0u;
    float best_ema = 0.0f;

    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        const ae_candidate_t *c = &ce->slots[i];
        if (!c->in_use || c->state != AE_CAND_ACTIVE) {
            continue;
        }
        if (c->promotion_entered) {
            continue;
        }
        if (c->quality_summary != AE_QUALITY_OK
            && c->quality_summary != AE_QUALITY_DEGRADED) {
            continue;
        }
        if (!ear_promotion_candidate_product_relevant(
                ce, c->candidate_id, NULL, NULL
            )) {
            continue;
        }
        if (!have
            || c->evidence_count > best_evidence
            || (c->evidence_count == best_evidence && c->ema_rms_dbfs > best_ema)
            || (c->evidence_count == best_evidence && c->ema_rms_dbfs == best_ema
                && c->candidate_id < best_id)) {
            have = true;
            best_id = c->candidate_id;
            best_evidence = c->evidence_count;
            best_ema = c->ema_rms_dbfs;
        }
    }

    if (!have) {
        return false;
    }
    *out_candidate_id = best_id;
    return true;
}

static void note_decision(
    ear_promotion_policy_t *pol,
    uint64_t cand,
    ear_promo_decision_t decision,
    uint16_t profile_id,
    float distance
)
{
    pol->diag.last_candidate_id = cand;
    pol->diag.last_decision = decision;
    pol->diag.last_profile_id = profile_id;
    pol->diag.last_match_distance = distance;
}

void ear_promotion_policy_poll(ear_promotion_policy_t *pol, int64_t now_mono_us)
{
    if (pol == NULL || pol->ce == NULL || pol->om == NULL) {
        return;
    }
    (void)now_mono_us;

    if (ear_obs_manager_n_held(pol->om) > 0u) {
        pol->diag.promote_skipped_busy++;
        note_decision(pol, 0u, EAR_PROMO_DECISION_HOLD_BUSY, 0u, 0.0f);
        return;
    }

    /*
     * Count soft-skips for diagnostics when no approved primary exists but
     * ambient ACTIVE candidates are present.
     */
    bool any_active = false;
    bool any_active_no_class = false;
    bool any_active_irrelevant = false;
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        const ae_candidate_t *c = &pol->ce->slots[i];
        if (!c->in_use || c->state != AE_CAND_ACTIVE || c->promotion_entered) {
            continue;
        }
        if (c->quality_summary != AE_QUALITY_OK
            && c->quality_summary != AE_QUALITY_DEGRADED) {
            continue;
        }
        any_active = true;
        ce_classification_t cls;
        if (!ce_get_classification(pol->ce, c->candidate_id, &cls) || !cls.valid) {
            any_active_no_class = true;
        } else if (!(cls.matched
                     && cls.profile_id == AS_PROFILE_ID_AS001
                     && cls.acoustic_class == AS_CLASS_HELICOPTER_LIKE)) {
            any_active_irrelevant = true;
        }
    }

    uint64_t primary = 0u;
    if (!ear_promotion_select_primary(pol->ce, &primary)) {
        pol->diag.select_none++;
        if (any_active_no_class) {
            pol->diag.hold_no_class++;
            note_decision(pol, 0u, EAR_PROMO_DECISION_HOLD_NO_CLASS, 0u, 0.0f);
        } else if (any_active_irrelevant || any_active) {
            pol->diag.hold_irrelevant++;
            note_decision(pol, 0u, EAR_PROMO_DECISION_HOLD_IRRELEVANT, 0u, 0.0f);
        } else {
            note_decision(pol, 0u, EAR_PROMO_DECISION_NONE, 0u, 0.0f);
        }
        return;
    }
    pol->diag.select_ok++;

    uint16_t profile_id = AS_PROFILE_ID_NONE;
    float distance = 0.0f;
    if (!ear_promotion_candidate_product_relevant(
            pol->ce, primary, &profile_id, &distance
        )) {
        /* Race: classification cleared after select — soft skip. */
        pol->diag.hold_irrelevant++;
        note_decision(
            pol, primary, EAR_PROMO_DECISION_HOLD_IRRELEVANT, profile_id, distance
        );
        return;
    }

    ear_obs_rsv_handle_t h;
    const ear_obs_rsv_result_t rr = ear_obs_manager_reserve(pol->om, &h);
    if (rr != EAR_OBS_RSV_OK) {
        pol->diag.reserve_full++;
        note_decision(
            pol, primary, EAR_PROMO_DECISION_HOLD_RESERVE_FULL, profile_id, distance
        );
        return;
    }
    pol->diag.reserve_ok++;

    if (!ear_obs_manager_bind_reservation_candidate(pol->om, h, primary)) {
        ear_obs_manager_release_reservation(pol->om, h);
        pol->diag.promote_fail++;
        note_decision(
            pol, primary, EAR_PROMO_DECISION_PROMOTE_FAIL, profile_id, distance
        );
        return;
    }

    /* Carry approval on the reservation before CE assigns promotion_gen. */
    if (!ear_obs_manager_stamp_reservation_relevance(
            pol->om, h, primary, profile_id, distance
        )) {
        ear_obs_manager_release_reservation(pol->om, h);
        pol->diag.promote_fail++;
        note_decision(
            pol, primary, EAR_PROMO_DECISION_PROMOTE_FAIL, profile_id, distance
        );
        return;
    }

    if (!ce_promote(pol->ce, primary)) {
        ear_obs_manager_release_reservation(pol->om, h);
        pol->diag.promote_fail++;
        note_decision(
            pol, primary, EAR_PROMO_DECISION_PROMOTE_FAIL, profile_id, distance
        );
        return;
    }
    pol->diag.promote_ok++;
    note_decision(pol, primary, EAR_PROMO_DECISION_PROMOTE, profile_id, distance);
    /* ENTER is queued into the manager; poll drains/commits. */
}

void ear_promotion_policy_get_diag(
    const ear_promotion_policy_t *pol,
    ear_promotion_policy_diag_t *out
)
{
    if (out == NULL) {
        return;
    }
    if (pol == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = pol->diag;
}
