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

#ifdef __cplusplus
}
#endif
