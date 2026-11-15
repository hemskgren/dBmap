#include "ear_promotion_policy.h"

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

bool ear_promotion_select_primary(const ce_engine_t *ce, uint64_t *out_candidate_id)
{
    if (ce == NULL || out_candidate_id == NULL || !ce->started) {
        return false;
    }

    /*
     * Multi-Obs (capacity 2): other PROMOTED / promotion_entered candidates do
     * not block selection. Skip any candidate that already entered promotion.
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

void ear_promotion_policy_poll(ear_promotion_policy_t *pol, int64_t now_mono_us)
{
    if (pol == NULL || pol->ce == NULL || pol->om == NULL) {
        return;
    }
    (void)now_mono_us;

    /*
     * One promote attempt per poll when no HELD reservation is outstanding.
     * Pending/open capacity is enforced by reserve().
     */
    if (ear_obs_manager_n_held(pol->om) > 0u) {
        pol->diag.promote_skipped_busy++;
        return;
    }

    uint64_t primary = 0u;
    if (!ear_promotion_select_primary(pol->ce, &primary)) {
        pol->diag.select_none++;
        return;
    }
    pol->diag.select_ok++;

    ear_obs_rsv_handle_t h;
    const ear_obs_rsv_result_t rr = ear_obs_manager_reserve(pol->om, &h);
    if (rr != EAR_OBS_RSV_OK) {
        pol->diag.reserve_full++;
        return;
    }
    pol->diag.reserve_ok++;

    if (!ear_obs_manager_bind_reservation_candidate(pol->om, h, primary)) {
        ear_obs_manager_release_reservation(pol->om, h);
        pol->diag.promote_fail++;
        return;
    }

    if (!ce_promote(pol->ce, primary)) {
        ear_obs_manager_release_reservation(pol->om, h);
        pol->diag.promote_fail++;
        return;
    }
    pol->diag.promote_ok++;
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
