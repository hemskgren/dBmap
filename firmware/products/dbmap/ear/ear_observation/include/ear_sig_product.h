/**
 * Production EAR wiring for the Signature Engine AE listener and the normal
 * post-AE poll. Device and host link this unit. Critical Path stays outside.
 *
 * Listener order on the Audio Engine remains the caller's responsibility:
 * this listener is registered before Candidate Engine.
 */
#pragma once

#include "ae_engine.h"
#include "ce_engine.h"
#include "ear_obs_lifecycle_section.h"
#include "ear_obs_manager.h"
#include "ear_promotion_policy.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ear_obs_manager_t *om;
    ear_promotion_policy_t *policy;
    ce_engine_t *ce;
    /**
     * When false, the listener still evaluates Signature Engine and skips
     * Observation Manager forwarding. Poll is a no-op.
     */
    bool ready;
} ear_sig_product_t;

/**
 * Product Signature Engine AE listener.
 * Evaluates the engine, then forwards the published AS-002 current-window
 * snapshot to Observation Manager absence tracking when ready.
 */
void ear_sig_product_on_ae_signature(const ae_signature_t *sig, void *user);

/**
 * Normal post-AE poll. Order:
 *   arm/feed siren evidence, Promotion Policy, Observation Manager,
 *   arm/feed again, candidate-associated evidence feed.
 * Does not poll Critical Path.
 */
void ear_sig_product_poll(ear_sig_product_t *prod, int64_t now_mono_us);

/**
 * One lifecycle-mutex section. before_poll runs under the lock, then
 * ear_sig_product_poll, then after_poll. START acceptance inside the poll
 * finishes before the section gives the mutex. Critical Path is not called.
 * The body must not take the same mutex.
 */
typedef struct ear_sig_product_section_args {
    ear_sig_product_t *prod;
    int64_t now_mono_us;
    void (*before_poll)(struct ear_sig_product_section_args *args);
    void (*after_poll)(struct ear_sig_product_section_args *args);
    void *before_ctx;
} ear_sig_product_section_args_t;

ear_obs_lifecycle_section_result_t ear_sig_product_poll_in_section(
    const ear_obs_lifecycle_lock_ops_t *lock,
    sp_obs_emit_gate_t *gate,
    ear_sig_product_section_args_t *args
);

#ifdef __cplusplus
}
#endif
