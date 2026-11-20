/**
 * Workflow classifier module: acoustic profile match (AS-001, …).
 *
 * Semantic classification only — does not mutate candidates or promote.
 * Prefers Signature Engine primary distance match (all defs, independent of
 * hit-vector truncation). Falls back to as_profile_match_best when the
 * engine is not started (legacy host tests).
 */
#include "ce_module.h"

#include "as_profile.h"
#include "sig_engine.h"

#include <string.h>

#define CE_MOD_FLAG_PROFILE_MATCH (1u << 8)

/** Legacy host-test fallback when Signature Engine is not started. */
static as_profile_bg_state_t s_legacy_bg;
static bool s_legacy_bg_ready;

static bool profile_init(void *ctx, const void *cfg)
{
    (void)ctx;
    (void)cfg;
    as_profile_bg_init(&s_legacy_bg);
    s_legacy_bg_ready = true;
    return true;
}

static void profile_shutdown(void *ctx)
{
    (void)ctx;
    as_profile_bg_reset(&s_legacy_bg);
    s_legacy_bg_ready = false;
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

    if (sig_engine_is_started()) {
        sig_primary_distance_t prim;
        if (sig_engine_get_primary_distance_match(&prim) && prim.valid
            && prim.window_seq == in->signature->window_seq) {
            out->score = prim.distance;
            out->code = (int32_t)prim.profile_id;
            out->flags = CE_MOD_FLAG_PROFILE_MATCH;
            out->status = CE_MOD_OK;
            return CE_MOD_OK;
        }
        out->score = 0.0f;
        out->code = 0;
        out->status = CE_MOD_OK;
        return CE_MOD_OK;
    }

    if (!s_legacy_bg_ready) {
        as_profile_bg_init(&s_legacy_bg);
        s_legacy_bg_ready = true;
    }
    as_profile_match_result_t hit;
    if (as_profile_match_best_with_bg(in->signature, &s_legacy_bg, &hit)
        && hit.matched) {
        out->score = hit.distance;
        out->code = (int32_t)hit.profile_id;
        out->flags = CE_MOD_FLAG_PROFILE_MATCH;
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
