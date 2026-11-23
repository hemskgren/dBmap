#include "ear_promotion_policy.h"

#include "as_profile.h"
#include "sig_engine.h"

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
    pol->siren = NULL;
    pol->as002_from_engine = false;
}

void ear_promotion_policy_set_siren(
    ear_promotion_policy_t *pol,
    const ear_siren_se_public_t *siren
)
{
    if (pol == NULL) {
        return;
    }
    pol->siren = siren;
    if (siren != NULL) {
        pol->as002_from_engine = false;
    }
}

void ear_promotion_policy_use_sig_engine_as002(
    ear_promotion_policy_t *pol,
    bool enable
)
{
    if (pol == NULL) {
        return;
    }
    pol->as002_from_engine = enable;
    if (enable) {
        pol->siren = NULL;
    }
}

static bool as002_promote_ok(
    const ear_promotion_policy_t *pol,
    const ae_candidate_t *candidate,
    float *out_consec
)
{
    if (pol == NULL) {
        return false;
    }
    if (pol->as002_from_engine) {
        /*
         * One coherent published snapshot for the whole decision — do not
         * call allows_promote and consecutive as separate locked reads.
         */
        sig_as002_snapshot_t snap;
        if (!sig_engine_copy_as002_snapshot(&snap) || !snap.valid) {
            return false;
        }
        if (snap.allows_promote == 0u
            || snap.last_window.kind != EAR_SIREN_LAST_USABLE
            || !snap.last_window.tonal_vote
            || (candidate != NULL && candidate->last_window_seq != snap.window_seq)) {
            return false;
        }
        if (out_consec != NULL) {
            *out_consec = (float)snap.consecutive;
        }
        return true;
    }
    if (pol->siren != NULL && ear_siren_se_public_allows_promote(pol->siren)) {
        if (out_consec != NULL) {
            *out_consec = (float)ear_siren_se_public_consecutive(pol->siren);
        }
        return true;
    }
    return false;
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

bool ear_promotion_candidate_promote_relevant(
    const ear_promotion_policy_t *pol,
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
    if (pol == NULL || pol->ce == NULL || candidate_id == 0u) {
        return false;
    }
    if (ear_promotion_candidate_product_relevant(
            pol->ce, candidate_id, out_profile_id, out_distance
        )) {
        return true;
    }
    {
        float consec = 0.0f;
        ae_candidate_t cand;
        /* Only the CE track that consumed this tonal window can claim it. */
        if (ce_find_candidate(pol->ce, candidate_id, &cand)
            && as002_promote_ok(pol, &cand, &consec)) {
            if (cand.quality_summary != AE_QUALITY_OK
                && cand.quality_summary != AE_QUALITY_DEGRADED) {
                return false;
            }
            if (out_profile_id != NULL) {
                *out_profile_id = AS_PROFILE_ID_AS002;
            }
            if (out_distance != NULL) {
                *out_distance = consec;
            }
            return true;
        }
    }
    return false;
}

bool ear_promotion_select_primary(
    const ear_promotion_policy_t *pol,
    uint64_t *out_candidate_id
)
{
    if (pol == NULL || pol->ce == NULL || out_candidate_id == NULL
        || !pol->ce->started) {
        return false;
    }

    /*
     * P2 deny-by-default: only product-relevant ACTIVE candidates compete.
     * Same relevance_profile_id already open/pending is excluded here so a
     * different signature can still promote into a free binding slot.
     */
    bool have = false;
    uint64_t best_id = 0u;
    uint32_t best_evidence = 0u;
    float best_ema = 0.0f;

    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        const ae_candidate_t *c = &pol->ce->slots[i];
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
        uint16_t profile_id = AS_PROFILE_ID_NONE;
        if (!ear_promotion_candidate_promote_relevant(
                pol, c->candidate_id, &profile_id, NULL
            )) {
            continue;
        }
        if (ear_obs_manager_has_signature_observation(pol->om, profile_id)) {
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
    const bool siren_ok = as002_promote_ok(pol, NULL, NULL);
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
        if (siren_ok) {
            /* The selection pass checks which candidate owns the tonal window. */
            continue;
        }
        ce_classification_t cls;
        if (!ce_get_classification(pol->ce, c->candidate_id, &cls) || !cls.valid) {
            any_active_no_class = true;
        } else if (!(cls.matched
                     && cls.profile_id == AS_PROFILE_ID_AS001
                     && cls.acoustic_class == AS_CLASS_HELICOPTER_LIKE)) {
            any_active_irrelevant = true;
        }
    }

    /*
     * Distinguish "no eligible primary" from "eligible but same signature
     * already open" for diagnostics (FARAN_OVER dual-candidate case).
     */
    bool any_same_signature_blocked = false;
    if (any_active || siren_ok) {
        for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
            const ae_candidate_t *c = &pol->ce->slots[i];
            if (!c->in_use || c->state != AE_CAND_ACTIVE || c->promotion_entered) {
                continue;
            }
            if (c->quality_summary != AE_QUALITY_OK
                && c->quality_summary != AE_QUALITY_DEGRADED) {
                continue;
            }
            uint16_t pid = AS_PROFILE_ID_NONE;
            if (!ear_promotion_candidate_promote_relevant(
                    pol, c->candidate_id, &pid, NULL
                )) {
                continue;
            }
            if (ear_obs_manager_has_signature_observation(pol->om, pid)) {
                any_same_signature_blocked = true;
                break;
            }
        }
    }

    uint64_t primary = 0u;
    if (!ear_promotion_select_primary(pol, &primary)) {
        pol->diag.select_none++;
        if (any_same_signature_blocked) {
            pol->diag.hold_signature_open++;
            note_decision(
                pol, 0u, EAR_PROMO_DECISION_HOLD_SIGNATURE_OPEN, 0u, 0.0f
            );
        } else if (any_active_no_class) {
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
    if (!ear_promotion_candidate_promote_relevant(
            pol, primary, &profile_id, &distance
        )) {
        /* Race: classification cleared after select — soft skip. */
        pol->diag.hold_irrelevant++;
        note_decision(
            pol, primary, EAR_PROMO_DECISION_HOLD_IRRELEVANT, profile_id, distance
        );
        return;
    }
    if (ear_obs_manager_has_signature_observation(pol->om, profile_id)) {
        /* Race: Obs opened after select — soft skip. */
        pol->diag.hold_signature_open++;
        note_decision(
            pol,
            primary,
            EAR_PROMO_DECISION_HOLD_SIGNATURE_OPEN,
            profile_id,
            distance
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
        (void)ear_obs_manager_release_reservation(pol->om, h);
        pol->diag.promote_fail++;
        note_decision(
            pol, primary, EAR_PROMO_DECISION_PROMOTE_FAIL, profile_id, distance
        );
        return;
    }
    if (!ear_obs_manager_stamp_reservation_relevance(
            pol->om, h, primary, profile_id, distance
        )) {
        (void)ear_obs_manager_release_reservation(pol->om, h);
        pol->diag.promote_fail++;
        note_decision(
            pol, primary, EAR_PROMO_DECISION_PROMOTE_FAIL, profile_id, distance
        );
        return;
    }

    if (!ce_promote(pol->ce, primary)) {
        (void)ear_obs_manager_release_reservation(pol->om, h);
        pol->diag.promote_fail++;
        note_decision(
            pol, primary, EAR_PROMO_DECISION_PROMOTE_FAIL, profile_id, distance
        );
        return;
    }
    pol->diag.promote_ok++;
    note_decision(pol, primary, EAR_PROMO_DECISION_PROMOTE, profile_id, distance);
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
