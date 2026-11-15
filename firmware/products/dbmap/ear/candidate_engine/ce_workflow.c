#include "ce_workflow.h"

#include <string.h>

static const ce_workflow_step_t k_track_steps[] = {
    {
        .kind = CE_WK_MODULE,
        .cond = CE_COND_ALWAYS,
        .on_missing = CE_DECISION_ERROR,
        .on_error = CE_DECISION_ERROR,
        .module_id = "ce.evidence",
    },
    {
        .kind = CE_WK_DECIDE,
        .cond = CE_COND_ALWAYS,
        .decide = CE_DECIDE_FUSE_TRACK,
    },
};

static const ce_workflow_t k_track = {
    .id = "ce.track",
    .steps = k_track_steps,
    .step_count = 2u,
};

const ce_workflow_t *ce_workflow_track(void)
{
    return &k_track;
}

static bool cond_holds(uint8_t cond, const ce_module_input_t *in)
{
    switch (cond) {
    case CE_COND_ALWAYS:
        return true;
    case CE_COND_HAS_SIGNATURE:
        return in != NULL && in->signature != NULL && !in->evidence_missing;
    case CE_COND_QUALITY_OK:
        return in != NULL && in->signature != NULL
            && in->signature->quality == AE_QUALITY_OK;
    case CE_COND_QUALITY_USABLE:
        return in != NULL && in->signature != NULL
            && (in->signature->quality == AE_QUALITY_OK
                || in->signature->quality == AE_QUALITY_DEGRADED);
    default:
        return false;
    }
}

static bool is_terminal(ce_decision_t d)
{
    return d != CE_DECISION_CONTINUE && d != CE_DECISION_SKIP;
}

static ce_decision_t map_status(
    ce_module_status_t st,
    uint8_t on_missing,
    uint8_t on_error
)
{
    if (st == CE_MOD_OK) {
        return CE_DECISION_CONTINUE;
    }
    if (st == CE_MOD_SKIPPED) {
        if (on_missing == CE_DECISION_CONTINUE) {
            return CE_DECISION_SKIP;
        }
        return (ce_decision_t)on_missing;
    }
    if (on_error == CE_DECISION_CONTINUE) {
        return CE_DECISION_ERROR;
    }
    return (ce_decision_t)on_error;
}

static void push_result(ce_workflow_result_t *out, const ce_module_result_t *result)
{
    if (out == NULL || result == NULL) {
        return;
    }
    if (out->result_count >= CE_WORKFLOW_MAX_RESULTS) {
        return;
    }
    out->results[out->result_count] = *result;
    out->result_count++;
}

static ce_decision_t exec_module(
    ce_runtime_t *rt,
    const ce_workflow_step_t *step,
    const char *module_id,
    const ce_module_input_t *in,
    ce_workflow_result_t *out
)
{
    ce_module_result_t result;
    const ce_module_status_t st = ce_runtime_execute(rt, module_id, in, &result);
    push_result(out, &result);
    if (out != NULL && out->modules_executed < 255u) {
        out->modules_executed++;
    }
    return map_status(st, step->on_missing, step->on_error);
}

void ce_workflow_run(
    ce_runtime_t *rt,
    const ce_workflow_t *wf,
    const ce_module_input_t *in,
    ce_workflow_result_t *out
)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->decision = CE_DECISION_ERROR;
    out->failed = true;

    if (rt == NULL || wf == NULL || wf->steps == NULL || in == NULL) {
        return;
    }
    if (wf->step_count > CE_WORKFLOW_MAX_STEPS) {
        return;
    }

    ce_decision_t acc = CE_DECISION_CONTINUE;
    bool stopped = false;

    for (uint8_t i = 0; i < wf->step_count; i++) {
        const ce_workflow_step_t *step = &wf->steps[i];
        if (!cond_holds(step->cond, in)) {
            if (out->steps_skipped < 255u) {
                out->steps_skipped++;
            }
            acc = CE_DECISION_SKIP;
            continue;
        }

        if (step->kind == CE_WK_MODULE || step->kind == CE_WK_PARALLEL) {
            const ce_decision_t d0 = exec_module(rt, step, step->module_id, in, out);
            if (step->kind == CE_WK_PARALLEL
                && step->module_id_b != NULL
                && step->module_id_b[0] != '\0') {
                const ce_decision_t d1 = exec_module(rt, step, step->module_id_b, in, out);
                if (d0 == CE_DECISION_ERROR || d1 == CE_DECISION_ERROR) {
                    acc = CE_DECISION_ERROR;
                    stopped = true;
                    break;
                }
                if (is_terminal(d0)) {
                    acc = d0;
                    stopped = true;
                    break;
                }
                if (is_terminal(d1)) {
                    acc = d1;
                    stopped = true;
                    break;
                }
                if (d0 == CE_DECISION_SKIP && d1 == CE_DECISION_SKIP) {
                    acc = CE_DECISION_SKIP;
                } else {
                    acc = CE_DECISION_CONTINUE;
                }
            } else {
                acc = d0;
                if (is_terminal(d0)) {
                    stopped = true;
                    break;
                }
            }
            continue;
        }

        if (step->kind == CE_WK_DECIDE) {
            if (step->decide == CE_DECIDE_FUSE_TRACK) {
                acc = ce_decision_fuse_track(in, out->results, out->result_count);
            } else if (step->decide < CE_DECISION_COUNT) {
                acc = (ce_decision_t)step->decide;
            } else {
                acc = CE_DECISION_ERROR;
            }
            if (acc == CE_DECISION_CONTINUE || acc == CE_DECISION_SKIP) {
                continue;
            }
            stopped = true;
            break;
        }

        acc = CE_DECISION_ERROR;
        stopped = true;
        break;
    }

    if (!stopped && acc == CE_DECISION_CONTINUE) {
        acc = CE_DECISION_KEEP;
    }
    out->decision = acc;
    out->failed = (acc == CE_DECISION_ERROR);
}
