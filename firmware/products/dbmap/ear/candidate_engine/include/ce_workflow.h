/**
 * Minimal workflow runner. Not a general workflow product.
 *
 * Programs are static step tables (no loader, no config language).
 * Parallel steps run on this thread in listed order so the result is
 * deterministic. They are independent of each other (no data dependency);
 * the current Ear activity task is single-threaded, so true concurrent
 * execution is not used.
 *
 * Conditions:
 *   CE_COND_ALWAYS
 *   CE_COND_HAS_SIGNATURE
 *   CE_COND_QUALITY_OK       — signature quality == OK
 *   CE_COND_QUALITY_USABLE   — OK or DEGRADED
 * A false condition skips the step (SKIP) and does not call the module.
 */
#pragma once

#include "ce_decision.h"
#include "ce_module.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CE_WORKFLOW_MAX_STEPS
#define CE_WORKFLOW_MAX_STEPS 8u
#endif

#ifndef CE_WORKFLOW_MAX_RESULTS
#define CE_WORKFLOW_MAX_RESULTS 8u
#endif

typedef enum {
    CE_WK_MODULE = 1,
    CE_WK_PARALLEL = 2,
    CE_WK_DECIDE = 3,
} ce_step_kind_t;

typedef enum {
    CE_COND_ALWAYS = 0,
    CE_COND_HAS_SIGNATURE = 1,
    CE_COND_QUALITY_OK = 2,
    CE_COND_QUALITY_USABLE = 3,
} ce_cond_t;

typedef struct {
    uint8_t kind;
    uint8_t cond;
    uint8_t on_missing; /* ce_decision_t; CONTINUE is treated as SKIP */
    uint8_t on_error;   /* ce_decision_t; CONTINUE is treated as ERROR */
    uint8_t decide;     /* ce_decision_t or CE_DECIDE_FUSE_TRACK */
    const char *module_id;
    const char *module_id_b;
} ce_workflow_step_t;

typedef struct {
    const char *id;
    const ce_workflow_step_t *steps;
    uint8_t step_count;
} ce_workflow_t;

typedef struct {
    ce_decision_t decision;
    uint8_t modules_executed;
    uint8_t steps_skipped;
    bool failed;
    ce_module_result_t results[CE_WORKFLOW_MAX_RESULTS];
    uint8_t result_count;
} ce_workflow_result_t;

/** Default track program: evidence module, then track fusion (KEEP/SKIP/ERROR). */
const ce_workflow_t *ce_workflow_track(void);

void ce_workflow_run(
    ce_runtime_t *rt,
    const ce_workflow_t *wf,
    const ce_module_input_t *in,
    ce_workflow_result_t *out
);

#ifdef __cplusplus
}
#endif
