/**
 * Static module runtime. No dynamic loader and no classification logic.
 *
 * The runtime owns module slot registration, init/shutdown, and execute
 * accounting. Modules receive a candidate snapshot and the AE signature
 * pointer; they must not store those pointers past execute, and they must
 * not change candidate state or call ce_promote / ce_reject.
 *
 * Input timestamps are copied from the signature only. A missing signature
 * leaves window_end_mono_us at 0 and does not substitute processing time.
 */
#pragma once

#include "ce_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CE_MAX_MODULES
/* evidence + as.profile built-ins, plus room for test/product modules. */
#define CE_MAX_MODULES 6u
#endif

#define CE_INPUT_SIGNATURE (1u << 0)
#define CE_INPUT_CANDIDATE (1u << 1)

#define CE_MOD_FLAG_GAP (1u << 0)
#define CE_MOD_FLAG_DEGRADED (1u << 1)
#define CE_MOD_FLAG_INVALID (1u << 2)
#define CE_MOD_FLAG_MISSING (1u << 3)

typedef enum {
    CE_MOD_OK = 0,
    CE_MOD_SKIPPED = 1,
    CE_MOD_ERROR = 2,
} ce_module_status_t;

typedef struct {
    const ae_signature_t *signature;
    const ae_candidate_t *candidate;
    uint64_t candidate_id;
    bool evidence_missing;
    bool evidence_degraded;
    bool evidence_gap;
    bool evidence_invalid;
    uint8_t quality;
    uint32_t window_seq;
    int64_t window_end_mono_us;
} ce_module_input_t;

typedef struct {
    ce_module_status_t status;
    int32_t code;
    float score;
    uint32_t flags;
    const char *module_id;
} ce_module_result_t;

typedef struct ce_module_desc {
    const char *id;
    uint16_t version;
    /** Bitmask of CE_INPUT_*. Unmet requirements skip the call. */
    uint32_t input_requirements;
    /**
     * Maximum execute() calls per runtime start. 0 means unlimited.
     * The cap is a resource limit, not a classification rule.
     */
    uint32_t max_exec;
    bool (*init)(void *ctx, const void *cfg);
    void (*shutdown)(void *ctx);
    ce_module_status_t (*execute)(
        void *ctx,
        const ce_module_input_t *in,
        ce_module_result_t *out
    );
    const void *cfg;
} ce_module_desc_t;

typedef struct {
    const ce_module_desc_t *desc;
    void *ctx;
    bool inited;
    uint32_t exec_count;
} ce_module_slot_t;

typedef struct {
    bool started;
    ce_module_slot_t slots[CE_MAX_MODULES];
    uint8_t count;
    uint32_t exec_total;
    uint32_t exec_errors;
    uint32_t exec_skipped;
    uint32_t missing;
} ce_runtime_t;

void ce_module_input_set(
    ce_module_input_t *in,
    const ae_signature_t *sig,
    const ae_candidate_t *cand
);

/** Built-in evidence probe. Reports quality flags. Score is always 0. */
const ce_module_desc_t *ce_module_evidence_desc(void);

/**
 * Acoustic profile classifier (AS-001, …). Score = match distance when
 * matched; code = profile id (0 = none). Never promotes. Flag bit 8 set on
 * match (CE_MOD_FLAG_PROFILE_MATCH in ce_module_profile.c).
 */
const ce_module_desc_t *ce_module_profile_desc(void);

void ce_runtime_reset(ce_runtime_t *rt);

/**
 * Register a statically linked descriptor. Fails on NULL, duplicate id,
 * missing init/shutdown/execute, or a full table.
 * If the runtime is already started, init() runs immediately.
 */
bool ce_runtime_register(ce_runtime_t *rt, const ce_module_desc_t *desc, void *ctx);

bool ce_runtime_start(ce_runtime_t *rt);
void ce_runtime_shutdown(ce_runtime_t *rt);

/**
 * Execute one module. Missing id → ERROR and missing++.
 * Unmet input requirements → SKIPPED without calling execute.
 * max_exec exhausted → ERROR without calling execute.
 */
ce_module_status_t ce_runtime_execute(
    ce_runtime_t *rt,
    const char *module_id,
    const ce_module_input_t *in,
    ce_module_result_t *out
);

bool ce_runtime_module_info(
    const ce_runtime_t *rt,
    const char *module_id,
    uint16_t *version_out,
    bool *inited_out
);

#ifdef __cplusplus
}
#endif
