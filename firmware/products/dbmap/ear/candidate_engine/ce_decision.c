#include "ce_decision.h"

#include <stddef.h>

const char *ce_decision_name(ce_decision_t d)
{
    switch (d) {
    case CE_DECISION_CONTINUE:
        return "CONTINUE";
    case CE_DECISION_SKIP:
        return "SKIP";
    case CE_DECISION_KEEP:
        return "KEEP";
    case CE_DECISION_PROMOTE:
        return "PROMOTE";
    case CE_DECISION_REJECT:
        return "REJECT";
    case CE_DECISION_ERROR:
        return "ERROR";
    default:
        return "INVALID";
    }
}

ce_decision_t ce_decision_fuse_track(
    const ce_module_input_t *in,
    const ce_module_result_t *results,
    uint8_t result_count
)
{
    if (in == NULL || in->evidence_missing || in->signature == NULL) {
        return CE_DECISION_ERROR;
    }
    if (in->evidence_invalid || in->signature->quality == AE_QUALITY_INVALID) {
        return CE_DECISION_ERROR;
    }

    if (results != NULL) {
        for (uint8_t i = 0; i < result_count; i++) {
            if (results[i].status == CE_MOD_ERROR
                || (results[i].flags & CE_MOD_FLAG_INVALID) != 0u) {
                return CE_DECISION_ERROR;
            }
        }
    }

    if (in->evidence_gap || in->signature->quality == AE_QUALITY_GAP) {
        return CE_DECISION_SKIP;
    }
    if (results != NULL) {
        for (uint8_t i = 0; i < result_count; i++) {
            if ((results[i].flags & CE_MOD_FLAG_GAP) != 0u) {
                return CE_DECISION_SKIP;
            }
        }
    }

    /* OK and DEGRADED are retained evidence. Score is not consulted. */
    return CE_DECISION_KEEP;
}
