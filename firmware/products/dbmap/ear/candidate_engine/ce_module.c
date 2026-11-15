#include "ce_module.h"

#include <string.h>

void ce_module_input_set(
    ce_module_input_t *in,
    const ae_signature_t *sig,
    const ae_candidate_t *cand
)
{
    if (in == NULL) {
        return;
    }
    memset(in, 0, sizeof(*in));
    in->signature = sig;
    in->candidate = cand;
    if (cand != NULL) {
        in->candidate_id = cand->candidate_id;
    }
    if (sig == NULL) {
        in->evidence_missing = true;
        in->quality = 0xFFu;
        return;
    }
    in->quality = sig->quality;
    in->window_seq = sig->window_seq;
    in->window_end_mono_us = sig->window_end_mono_us;
    if (sig->quality == AE_QUALITY_GAP) {
        in->evidence_gap = true;
    } else if (sig->quality == AE_QUALITY_INVALID) {
        in->evidence_invalid = true;
    } else if (sig->quality == AE_QUALITY_DEGRADED) {
        in->evidence_degraded = true;
    }
}

static bool evidence_init(void *ctx, const void *cfg)
{
    (void)ctx;
    (void)cfg;
    return true;
}

static void evidence_shutdown(void *ctx)
{
    (void)ctx;
}

static ce_module_status_t evidence_execute(
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
    out->module_id = "ce.evidence";
    out->score = 0.0f;
    if (in == NULL || in->signature == NULL || in->evidence_missing) {
        out->status = CE_MOD_SKIPPED;
        out->flags = CE_MOD_FLAG_MISSING;
        return CE_MOD_SKIPPED;
    }
    if (in->evidence_gap) {
        out->flags |= CE_MOD_FLAG_GAP;
    }
    if (in->evidence_degraded) {
        out->flags |= CE_MOD_FLAG_DEGRADED;
    }
    if (in->evidence_invalid) {
        out->flags |= CE_MOD_FLAG_INVALID;
    }
    out->status = CE_MOD_OK;
    return CE_MOD_OK;
}

static const ce_module_desc_t k_evidence = {
    .id = "ce.evidence",
    .version = 1u,
    .input_requirements = CE_INPUT_SIGNATURE,
    .max_exec = 0u,
    .init = evidence_init,
    .shutdown = evidence_shutdown,
    .execute = evidence_execute,
    .cfg = NULL,
};

const ce_module_desc_t *ce_module_evidence_desc(void)
{
    return &k_evidence;
}

void ce_runtime_reset(ce_runtime_t *rt)
{
    if (rt == NULL) {
        return;
    }
    memset(rt, 0, sizeof(*rt));
}

static ce_module_slot_t *find_slot(ce_runtime_t *rt, const char *module_id)
{
    if (rt == NULL || module_id == NULL) {
        return NULL;
    }
    for (uint8_t i = 0; i < rt->count; i++) {
        const ce_module_desc_t *desc = rt->slots[i].desc;
        if (desc != NULL && desc->id != NULL && strcmp(desc->id, module_id) == 0) {
            return &rt->slots[i];
        }
    }
    return NULL;
}

bool ce_runtime_register(ce_runtime_t *rt, const ce_module_desc_t *desc, void *ctx)
{
    if (rt == NULL || desc == NULL || desc->id == NULL || desc->id[0] == '\0') {
        return false;
    }
    if (desc->init == NULL || desc->shutdown == NULL || desc->execute == NULL) {
        return false;
    }
    if (find_slot(rt, desc->id) != NULL) {
        return false;
    }
    if (rt->count >= CE_MAX_MODULES) {
        return false;
    }

    ce_module_slot_t *slot = &rt->slots[rt->count];
    memset(slot, 0, sizeof(*slot));
    slot->desc = desc;
    slot->ctx = ctx;
    if (rt->started) {
        if (!desc->init(ctx, desc->cfg)) {
            memset(slot, 0, sizeof(*slot));
            return false;
        }
        slot->inited = true;
    }
    rt->count++;
    return true;
}

bool ce_runtime_start(ce_runtime_t *rt)
{
    if (rt == NULL || rt->started) {
        return false;
    }
    for (uint8_t i = 0; i < rt->count; i++) {
        ce_module_slot_t *slot = &rt->slots[i];
        if (slot->desc == NULL || slot->desc->init == NULL
            || !slot->desc->init(slot->ctx, slot->desc->cfg)) {
            for (uint8_t j = 0; j < i; j++) {
                if (rt->slots[j].inited && rt->slots[j].desc != NULL
                    && rt->slots[j].desc->shutdown != NULL) {
                    rt->slots[j].desc->shutdown(rt->slots[j].ctx);
                }
                rt->slots[j].inited = false;
            }
            return false;
        }
        slot->inited = true;
    }
    rt->started = true;
    return true;
}

void ce_runtime_shutdown(ce_runtime_t *rt)
{
    if (rt == NULL) {
        return;
    }
    for (int i = (int)rt->count - 1; i >= 0; i--) {
        ce_module_slot_t *slot = &rt->slots[i];
        if (slot->inited && slot->desc != NULL && slot->desc->shutdown != NULL) {
            slot->desc->shutdown(slot->ctx);
        }
        slot->inited = false;
    }
    rt->started = false;
}

static void result_error(ce_module_result_t *out, const char *module_id)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->status = CE_MOD_ERROR;
    out->module_id = module_id;
}

ce_module_status_t ce_runtime_execute(
    ce_runtime_t *rt,
    const char *module_id,
    const ce_module_input_t *in,
    ce_module_result_t *out
)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->module_id = module_id;
    }
    if (rt == NULL || !rt->started || module_id == NULL) {
        if (rt != NULL) {
            rt->missing++;
        }
        result_error(out, module_id);
        return CE_MOD_ERROR;
    }

    ce_module_slot_t *slot = find_slot(rt, module_id);
    if (slot == NULL || slot->desc == NULL || slot->desc->execute == NULL) {
        rt->missing++;
        result_error(out, module_id);
        return CE_MOD_ERROR;
    }

    const uint32_t need = slot->desc->input_requirements;
    const bool sig_missing = (in == NULL || in->signature == NULL || in->evidence_missing);
    const bool cand_missing = (in == NULL || in->candidate == NULL);
    if (((need & CE_INPUT_SIGNATURE) != 0u && sig_missing)
        || ((need & CE_INPUT_CANDIDATE) != 0u && cand_missing)) {
        rt->exec_skipped++;
        if (out != NULL) {
            out->status = CE_MOD_SKIPPED;
            out->flags = CE_MOD_FLAG_MISSING;
            out->module_id = slot->desc->id;
        }
        return CE_MOD_SKIPPED;
    }

    if (slot->desc->max_exec != 0u && slot->exec_count >= slot->desc->max_exec) {
        rt->exec_errors++;
        rt->exec_total++;
        if (out != NULL) {
            out->status = CE_MOD_ERROR;
            out->code = 1;
            out->module_id = slot->desc->id;
        }
        return CE_MOD_ERROR;
    }

    ce_module_result_t local;
    ce_module_result_t *dest = out != NULL ? out : &local;
    memset(dest, 0, sizeof(*dest));
    dest->module_id = slot->desc->id;
    const ce_module_status_t st = slot->desc->execute(slot->ctx, in, dest);
    dest->status = st;
    dest->module_id = slot->desc->id;
    slot->exec_count++;
    rt->exec_total++;
    if (st == CE_MOD_ERROR) {
        rt->exec_errors++;
    } else if (st == CE_MOD_SKIPPED) {
        rt->exec_skipped++;
    }
    return st;
}

bool ce_runtime_module_info(
    const ce_runtime_t *rt,
    const char *module_id,
    uint16_t *version_out,
    bool *inited_out
)
{
    if (rt == NULL || module_id == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < rt->count; i++) {
        const ce_module_desc_t *desc = rt->slots[i].desc;
        if (desc != NULL && desc->id != NULL && strcmp(desc->id, module_id) == 0) {
            if (version_out != NULL) {
                *version_out = desc->version;
            }
            if (inited_out != NULL) {
                *inited_out = rt->slots[i].inited;
            }
            return true;
        }
    }
    return false;
}
