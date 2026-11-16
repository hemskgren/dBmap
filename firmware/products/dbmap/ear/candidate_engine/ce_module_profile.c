/**
 * Workflow classifier module: acoustic profile match (AS-001, …).
 *
 * Semantic classification only — does not mutate candidates or promote.
 * Match algorithm lives in acoustic_profile; this module adapts it to CE
 * module_result_t for the track workflow.
 */
#include "ce_module.h"

#include "as_profile.h"

#include <string.h>

#define CE_MOD_FLAG_PROFILE_MATCH (1u << 8)

static bool profile_init(void *ctx, const void *cfg)
{
    (void)ctx;
    (void)cfg;
    return true;
}

static void profile_shutdown(void *ctx)
{
    (void)ctx;
}

static ce_module_status_t profile_execute(
    void *ctx,
    const ce_module_input_t *in,
    ce_module_result_t *out
)
{
    (void)ctx;
    if (out == NULL) {
        return CE_MOD_ERROR;
    }
    memset(out, 0, sizeof(*out));
    out->module_id = "as.profile";
    out->score = 0.0f;
    out->code = 0;

    if (in == NULL || in->signature == NULL || in->evidence_missing) {
        out->status = CE_MOD_SKIPPED;
        out->flags = CE_MOD_FLAG_MISSING;
        return CE_MOD_SKIPPED;
    }
    if (in->evidence_invalid) {
        out->status = CE_MOD_SKIPPED;
        out->flags = CE_MOD_FLAG_INVALID;
        return CE_MOD_SKIPPED;
    }
    if (in->evidence_gap) {
        out->status = CE_MOD_SKIPPED;
        out->flags = CE_MOD_FLAG_GAP;
        return CE_MOD_SKIPPED;
    }

    as_profile_match_result_t hit;
    if (as_profile_match_best(in->signature, &hit) && hit.matched) {
        out->score = hit.distance;
        out->code = (int32_t)hit.profile_id;
        out->flags = CE_MOD_FLAG_PROFILE_MATCH;
        /* Pack acoustic class in the high 16 bits of unused flag space via code
         * is insufficient; callers use as_profile_get_by_id(profile_id)->class. */
    } else {
        out->score = hit.distance;
        out->code = 0;
    }
    out->status = CE_MOD_OK;
    return CE_MOD_OK;
}

static const ce_module_desc_t k_profile = {
    .id = "as.profile",
    .version = 1u,
    .input_requirements = CE_INPUT_SIGNATURE,
    .max_exec = 0u,
    .init = profile_init,
    .shutdown = profile_shutdown,
    .execute = profile_execute,
    .cfg = NULL,
};

const ce_module_desc_t *ce_module_profile_desc(void)
{
    return &k_profile;
}
