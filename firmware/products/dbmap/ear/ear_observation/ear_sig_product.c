/**
 * Shared product wiring for Signature Engine → Observation Manager.
 * Host tests and ear_observation.c both call these functions.
 */
#include "ear_sig_product.h"

#include "ear_obs_lifecycle_section.h"
#include "sig_engine.h"

static void forward_as002_absence(ear_obs_manager_t *om)
{
    /*
     * AS002 signature-absence END uses current-window tonal_vote only
     * (global siren timbre support — not source-specific identity).
     * Reads the published AS-002 snapshot from this listener's evaluation.
     */
    ear_siren_last_window_t lw;
    if (!sig_engine_copy_as002_last_window(&lw)) {
        return;
    }
    if (lw.kind == EAR_SIREN_LAST_NONE) {
        return;
    }
    const bool usable = (lw.kind == EAR_SIREN_LAST_USABLE);
    ear_obs_manager_on_as002_siren_window(
        om,
        usable,
        usable && lw.tonal_vote,
        lw.window_ms,
        lw.window_end_mono_us,
        lw.has_rms_dbfs,
        lw.rms_dbfs
    );
}

static void apply_siren_cls_evidence(ear_obs_manager_t *om)
{
    ear_cls_evidence_t ev;
    if (!sig_engine_copy_as002_evidence(&ev)) {
        return;
    }
    ear_obs_manager_arm_cls_evidence(om, &ev);
    /*
     * Attempt every open binding. Observation Manager accepts the write
     * only for an approved AS-002 binding.
     */
    for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (!om->bind[i].in_use) {
            continue;
        }
        (void)ear_obs_manager_feed_cls_evidence(
            om,
            om->bind[i].candidate_id,
            &ev,
            EAR_CLS_PHASE_PERIODIC
        );
    }
}

void ear_sig_product_on_ae_signature(const ae_signature_t *sig, void *user)
{
    if (sig == NULL) {
        return;
    }
    sig_engine_on_signature(sig);
    ear_sig_product_t *prod = (ear_sig_product_t *)user;
    if (prod == NULL || !prod->ready || prod->om == NULL) {
        return;
    }
    forward_as002_absence(prod->om);
}

static void product_poll_section_body(void *ctx)
{
    ear_sig_product_section_args_t *args = ctx;
    if (args == NULL) {
        return;
    }
    if (args->before_poll != NULL) {
        args->before_poll(args);
    }
    if (args->prod != NULL) {
        ear_sig_product_poll(args->prod, args->now_mono_us);
    }
    if (args->after_poll != NULL) {
        args->after_poll(args);
    }
}

ear_obs_lifecycle_section_result_t ear_sig_product_poll_in_section(
    const ear_obs_lifecycle_lock_ops_t *lock,
    sp_obs_emit_gate_t *gate,
    ear_sig_product_section_args_t *args
)
{
    return ear_obs_lifecycle_section(lock, gate, product_poll_section_body, args);
}

void ear_sig_product_poll(ear_sig_product_t *prod, int64_t now_mono_us)
{
    if (prod == NULL || !prod->ready || prod->om == NULL || prod->policy == NULL) {
        return;
    }
    /* Siren evidence was updated by the AE listener during publish. */
    apply_siren_cls_evidence(prod->om);
    ear_promotion_policy_poll(prod->policy, now_mono_us);
    ear_obs_manager_poll(prod->om, now_mono_us);
    /* Re-apply after ENTER so a START-armed binding can refine same cycle. */
    apply_siren_cls_evidence(prod->om);
    if (prod->ce != NULL) {
        ear_obs_manager_feed_from_ce(prod->om, prod->ce);
    }
}
