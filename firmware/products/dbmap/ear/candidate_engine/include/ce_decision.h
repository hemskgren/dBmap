/**
 * Decision vocabulary for the Candidate Engine.
 *
 * A classification module does not emit these values. It returns
 * ce_module_result_t evidence. Decision/Fusion (and an explicit DECIDE step)
 * produces a ce_decision_t. The Workflow engine sequences steps. The Candidate
 * Manager is the only component that changes candidate lifecycle, and only for
 * PROMOTE and REJECT.
 *
 * CONTINUE — The step finished normally. Proceed to the next step.
 *   Produced by the workflow after a module status of OK (or an explicit
 *   non-terminal DECIDE). Acted on by the workflow only.
 *
 * SKIP — This step does not apply, or the signature is a gap rather than
 *   continuous evidence. The candidate is not promoted or rejected.
 *   Produced by the workflow when a condition is false, a module returns
 *   SKIPPED, or a GAP signature is short-circuited before modules run.
 *   Acted on by the workflow only.
 *
 * KEEP — Retain the candidate. Association/update has already been applied.
 *   Do not promote and do not reject.
 *   Produced by Decision/Fusion (track fusion returns KEEP for OK and
 *   DEGRADED evidence) or an explicit DECIDE KEEP. Modules cannot emit KEEP.
 *   Acted on by the Candidate Manager as "no further transition".
 *
 * PROMOTE — Request promotion through the existing ce_promote contract
 *   (ENTER callback, then the Ear promotion bridge).
 *   Produced only by an explicit DECIDE PROMOTE, or by the existing bridge
 *   calling ce_promote directly. Track fusion never returns PROMOTE.
 *   A positive module score is not a promotion.
 *   Acted on by the Candidate Manager: ACTIVE or COASTING only, and only when
 *   promotion_entered is false. Otherwise the request is declined and no
 *   second ENTER is emitted. NASCENT is not promoted.
 *
 * REJECT — Terminal rejection. The candidate must not be promoted later.
 *   Produced by an explicit DECIDE REJECT or by ce_reject. Modules cannot
 *   emit REJECT. Track fusion never returns REJECT.
 *   Acted on by the Candidate Manager: one EXIT if promotion_entered, then
 *   the slot is terminal (REJECTED, not in use).
 *
 * ERROR — Module, workflow, or evidence failure. Do not promote or reject.
 *   Produced by the workflow when a module is missing or returns ERROR, when
 *   required evidence is missing, or when the signature is unusable
 *   (ae_signature_usable_for_tracking is false, including AE_QUALITY_INVALID).
 *   Acted on by the Candidate Manager as "no lifecycle change".
 *   workflow_failed counts workflow/module failures. ignored_unusable counts
 *   the unusable-signature short-circuit; that path does not run modules.
 */
#pragma once

#include "ce_module.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CE_DECISION_CONTINUE = 0,
    CE_DECISION_SKIP = 1,
    CE_DECISION_KEEP = 2,
    CE_DECISION_PROMOTE = 3,
    CE_DECISION_REJECT = 4,
    CE_DECISION_ERROR = 5,
    CE_DECISION_COUNT = 6,
} ce_decision_t;

/** Step decide-field sentinel: fuse module results with the track policy. */
#define CE_DECIDE_FUSE_TRACK 0xFFu

const char *ce_decision_name(ce_decision_t d);

/**
 * Track fusion. Reads module status/flags and the evidence quality already
 * on the signature. Ignores score. Never returns PROMOTE or REJECT.
 * OK and DEGRADED → KEEP. GAP → SKIP. Missing or INVALID → ERROR.
 * Any module result with status ERROR → ERROR.
 */
ce_decision_t ce_decision_fuse_track(
    const ce_module_input_t *in,
    const ce_module_result_t *results,
    uint8_t result_count
);

#ifdef __cplusplus
}
#endif
