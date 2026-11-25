#include "ear_promotion_policy.h"

#include "as_profile.h"
#include "sig_engine.h"

#include <string.h>

#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
#include "esp_log.h"
typedef struct {
    const char *as001;
    const char *as002;
    const char *selection;
    uint64_t candidate_id;
    uint32_t candidate_seq;
    uint32_t siren_seq;
} policy_trace_t;
#define TRACE_PARAM , policy_trace_t *trace
#define TRACE_ARG , trace
#define TRACE_NONE , NULL
#define TRACE_SET(field, value) do { if (trace != NULL) trace->field = (value); } while (0)
#else
#define TRACE_PARAM
#define TRACE_ARG
#define TRACE_NONE
#define TRACE_SET(field, value) ((void)0)
#endif

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
    float *out_consec TRACE_PARAM
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
            TRACE_SET(as002, "no_snapshot");
            return false;
        }
        TRACE_SET(siren_seq, snap.window_seq);
        if (snap.allows_promote == 0u
            || snap.last_window.kind != EAR_SIREN_LAST_USABLE
            || !snap.last_window.tonal_vote) {
            TRACE_SET(as002, "not_qualified");
            return false;
        }
        if (candidate != NULL && candidate->last_window_seq != snap.window_seq) {
            TRACE_SET(as002, "owner_mismatch");
            return false;
        }
        TRACE_SET(as002, "qualified");
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

static bool candidate_product_relevant(
    const ce_engine_t *ce,
    uint64_t candidate_id,
    uint16_t *out_profile_id,
    float *out_distance TRACE_PARAM
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
        TRACE_SET(as001, "no_class");
        return false;
    }
    if (!cls.matched
        || cls.profile_id != AS_PROFILE_ID_AS001
        || cls.acoustic_class != AS_CLASS_HELICOPTER_LIKE) {
        TRACE_SET(as001, "wrong_class");
        return false;
    }
    /* Freshness: annotation must post-date any GAP on this candidate. */
    ae_candidate_t cand;
    if (!ce_find_candidate(ce, candidate_id, &cand)
        || cls.gap_events_at_match != cand.gap_events) {
        TRACE_SET(as001, "stale_after_gap");
        return false;
    }
    TRACE_SET(as001, "qualified");
    if (out_profile_id != NULL) {
        *out_profile_id = cls.profile_id;
    }
    if (out_distance != NULL) {
        *out_distance = cls.distance;
    }
    return true;
}

bool ear_promotion_candidate_product_relevant(
    const ce_engine_t *ce, uint64_t candidate_id,
    uint16_t *out_profile_id, float *out_distance
)
{
    return candidate_product_relevant(ce, candidate_id, out_profile_id, out_distance TRACE_NONE);
}

static bool candidate_promote_relevant(
    const ear_promotion_policy_t *pol,
    uint64_t candidate_id,
    uint16_t *out_profile_id,
    float *out_distance TRACE_PARAM
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
    if (candidate_product_relevant(
            pol->ce, candidate_id, out_profile_id, out_distance TRACE_ARG
        )) {
        return true;
    }
    {
        float consec = 0.0f;
        ae_candidate_t cand;
        /* Only the CE track that consumed this tonal window can claim it. */
        if (ce_find_candidate(pol->ce, candidate_id, &cand)
            && as002_promote_ok(pol, &cand, &consec TRACE_ARG)) {
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

bool ear_promotion_candidate_promote_relevant(
    const ear_promotion_policy_t *pol, uint64_t candidate_id,
    uint16_t *out_profile_id, float *out_distance
)
{
    return candidate_promote_relevant(pol, candidate_id, out_profile_id, out_distance TRACE_NONE);
}

static bool select_primary(
    const ear_promotion_policy_t *pol,
    uint64_t *out_candidate_id TRACE_PARAM
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
#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
        policy_trace_t evaluated = {
            .as001 = "-", .as002 = "-", .selection = "ineligible",
            .candidate_id = c->candidate_id, .candidate_seq = c->last_window_seq,
        };
#define TRACE_SKIPPED(reason) \
        do { \
            if (trace != NULL && c->in_use && trace->candidate_id == 0u) { \
                evaluated.selection = (reason); \
                *trace = evaluated; \
            } \
        } while (0)
#else
#define TRACE_SKIPPED(reason) ((void)0)
#endif
        if (!c->in_use || c->state != AE_CAND_ACTIVE) {
            TRACE_SKIPPED("not_active");
            continue;
        }
        if (c->promotion_entered) {
            TRACE_SKIPPED("already_entered");
            continue;
        }
        if (c->quality_summary != AE_QUALITY_OK
            && c->quality_summary != AE_QUALITY_DEGRADED) {
            TRACE_SKIPPED("quality");
            continue;
        }
#undef TRACE_SKIPPED
        uint16_t profile_id = AS_PROFILE_ID_NONE;
        if (!candidate_promote_relevant(
                pol, c->candidate_id, &profile_id, NULL
#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
                , trace != NULL ? &evaluated : NULL
#endif
            )) {
#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
            if (trace != NULL && (strcmp(trace->as001, "-") == 0
                || (strcmp(trace->selection, "ineligible") == 0
                    && (strcmp(evaluated.as002, "owner_mismatch") == 0
                        || strcmp(evaluated.as001, "stale_after_gap") == 0)))) {
                *trace = evaluated;
            }
#endif
            continue;
        }
        if (ear_obs_manager_has_signature_observation(pol->om, profile_id)) {
#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
            if (trace != NULL && !have) {
                evaluated.selection = "profile_open";
                *trace = evaluated;
            }
#endif
            continue;
        }
        if (!have
            || c->evidence_count > best_evidence
            || (c->evidence_count == best_evidence && c->ema_rms_dbfs > best_ema)
            || (c->evidence_count == best_evidence && c->ema_rms_dbfs == best_ema
                && c->candidate_id < best_id)) {
#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
            if (trace != NULL) {
                evaluated.selection = "selected";
                *trace = evaluated;
            }
#endif
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

bool ear_promotion_select_primary(
    const ear_promotion_policy_t *pol, uint64_t *out_candidate_id
)
{
    return select_primary(pol, out_candidate_id TRACE_NONE);
}

static void note_decision(
    ear_promotion_policy_t *pol,
    uint64_t cand,
    ear_promo_decision_t decision,
    uint16_t profile_id,
    float distance TRACE_PARAM
)
{
    pol->diag.last_candidate_id = cand;
    pol->diag.last_decision = decision;
    pol->diag.last_profile_id = profile_id;
    pol->diag.last_match_distance = distance;
#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
    const uint32_t seq = pol->ce->last_processed_seq;
    if (trace != NULL && seq != 0u && seq != pol->last_logged_window) {
        pol->last_logged_window = seq;
        ESP_LOGI("ear_dec", "policy seq=%lu cid=%llu cseq=%lu sseq=%lu as001=%s as002=%s sel=%s decision=%u profile=%u",
                 (unsigned long)seq, (unsigned long long)trace->candidate_id,
                 (unsigned long)trace->candidate_seq, (unsigned long)trace->siren_seq,
                 trace->as001, trace->as002, trace->selection,
                 (unsigned)decision, (unsigned)profile_id);
    }
#endif
}

void ear_promotion_policy_poll(ear_promotion_policy_t *pol, int64_t now_mono_us)
{
    if (pol == NULL || pol->ce == NULL || pol->om == NULL) {
        return;
    }
    (void)now_mono_us;
#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
    policy_trace_t evaluation = {.as001 = "-", .as002 = "-", .selection = "no_eligible_track"};
    policy_trace_t *trace = pol->ce->last_processed_seq != pol->last_logged_window
                            ? &evaluation : NULL;
#endif

    if (ear_obs_manager_n_held(pol->om) > 0u) {
        TRACE_SET(selection, "busy");
        pol->diag.promote_skipped_busy++;
        note_decision(pol, 0u, EAR_PROMO_DECISION_HOLD_BUSY, 0u, 0.0f TRACE_ARG);
        return;
    }

    /*
     * Count soft-skips for diagnostics when no approved primary exists but
     * ambient ACTIVE candidates are present.
     */
    bool any_active = false;
    bool any_active_no_class = false;
    bool any_active_irrelevant = false;
    const bool siren_ok = as002_promote_ok(pol, NULL, NULL TRACE_ARG);
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
    if (!select_primary(pol, &primary TRACE_ARG)) {
        pol->diag.select_none++;
        if (any_same_signature_blocked) {
            pol->diag.hold_signature_open++;
            note_decision(
                pol, 0u, EAR_PROMO_DECISION_HOLD_SIGNATURE_OPEN, 0u, 0.0f TRACE_ARG
            );
        } else if (any_active_no_class) {
            pol->diag.hold_no_class++;
            note_decision(pol, 0u, EAR_PROMO_DECISION_HOLD_NO_CLASS, 0u, 0.0f TRACE_ARG);
        } else if (any_active_irrelevant || any_active) {
            pol->diag.hold_irrelevant++;
            note_decision(pol, 0u, EAR_PROMO_DECISION_HOLD_IRRELEVANT, 0u, 0.0f TRACE_ARG);
        } else {
            note_decision(pol, 0u, EAR_PROMO_DECISION_NONE, 0u, 0.0f TRACE_ARG);
        }
        return;
    }
    pol->diag.select_ok++;

    uint16_t profile_id = AS_PROFILE_ID_NONE;
    float distance = 0.0f;
    if (!candidate_promote_relevant(
            pol, primary, &profile_id, &distance TRACE_ARG
        )) {
        /* Race: classification cleared after select — soft skip. */
        TRACE_SET(selection, "relevance_changed");
        pol->diag.hold_irrelevant++;
        note_decision(
            pol, primary, EAR_PROMO_DECISION_HOLD_IRRELEVANT, profile_id, distance TRACE_ARG
        );
        return;
    }
    if (ear_obs_manager_has_signature_observation(pol->om, profile_id)) {
        /* Race: Obs opened after select — soft skip. */
        TRACE_SET(selection, "profile_open");
        pol->diag.hold_signature_open++;
        note_decision(
            pol,
            primary,
            EAR_PROMO_DECISION_HOLD_SIGNATURE_OPEN,
            profile_id,
            distance TRACE_ARG
        );
        return;
    }

    ear_obs_rsv_handle_t h;
    const ear_obs_rsv_result_t rr = ear_obs_manager_reserve(pol->om, &h);
    if (rr != EAR_OBS_RSV_OK) {
        TRACE_SET(selection, "reserve_full");
        pol->diag.reserve_full++;
        note_decision(
            pol, primary, EAR_PROMO_DECISION_HOLD_RESERVE_FULL, profile_id, distance TRACE_ARG
        );
        return;
    }
    pol->diag.reserve_ok++;

    if (!ear_obs_manager_bind_reservation_candidate(pol->om, h, primary)) {
        TRACE_SET(selection, "bind_failed");
        (void)ear_obs_manager_release_reservation(pol->om, h);
        pol->diag.promote_fail++;
        note_decision(
            pol, primary, EAR_PROMO_DECISION_PROMOTE_FAIL, profile_id, distance TRACE_ARG
        );
        return;
    }
    if (!ear_obs_manager_stamp_reservation_relevance(
            pol->om, h, primary, profile_id, distance
        )) {
        TRACE_SET(selection, "stamp_failed");
        (void)ear_obs_manager_release_reservation(pol->om, h);
        pol->diag.promote_fail++;
        note_decision(
            pol, primary, EAR_PROMO_DECISION_PROMOTE_FAIL, profile_id, distance TRACE_ARG
        );
        return;
    }

    if (!ce_promote(pol->ce, primary)) {
        TRACE_SET(selection, "ce_promote_failed");
        (void)ear_obs_manager_release_reservation(pol->om, h);
        pol->diag.promote_fail++;
        note_decision(
            pol, primary, EAR_PROMO_DECISION_PROMOTE_FAIL, profile_id, distance TRACE_ARG
        );
        return;
    }
    pol->diag.promote_ok++;
    note_decision(pol, primary, EAR_PROMO_DECISION_PROMOTE, profile_id, distance TRACE_ARG);
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
