/**
 * Candidate Engine façade.
 *
 * Owns module runtime lifetime, the active workflow, and the AE listener
 * binding. Candidate slot mutations belong to the Candidate Manager
 * (ce_manager.h). Ear product policy may scan slots[] read-only for ranking.
 *
 * Default workflow is ce_workflow_track(): evidence flags, then KEEP / SKIP /
 * ERROR. It does not promote or reject. Device promotion stays on
 * ce_promotion_bridge → ce_promote (or a later Ear Observation Manager that
 * calls the same actuator). ce_on_signature remains AE-listener compatible;
 * ce_bind_audio_engine is the preferred bind helper when the product owns
 * listener lifetime.
 *
 * ce_start() memsets the object and does not call module shutdown. Call
 * ce_stop() before starting the same object again so listeners are removed
 * and modules are shut down.
 *
 * ce_stop() safe semantics:
 * 1. Remove the AE listener (no further signatures).
 * 2. Emit one CE_PROMOTION_EXIT for each promotion_entered candidate so the
 *    bridge (or Observation Manager) can clear association when it polls.
 * 3. Clear all candidate slots (they become unavailable; not coast-expiry).
 * 4. Shut modules down and mark the engine stopped.
 * Stop does not open or end Observations. EXIT clears CE promotion binding
 * only; Observation END remains with Ear Observation ownership.
 */
#pragma once

#include "ae_engine.h"
#include "ce_module.h"
#include "ce_types.h"
#include "ce_workflow.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CE_MAX_CANDIDATES
#define CE_MAX_CANDIDATES 8u
#endif

typedef struct {
    uint32_t n_enter;           /* NASCENT → ACTIVE */
    uint32_t t_coast;           /* windows in COASTING before EXPIRED */
    /**
     * Associate (rematch) if identity distance <= this.
     * Create requires distance > match_distance_max + match_hysteresis.
     */
    float match_distance_max;
    /**
     * Hysteresis margin above match_distance_max. Signatures in
     * (match_max, match_max + hysteresis] neither update nor create.
     */
    float match_hysteresis;
    /**
     * When best and second-best are both <= match_distance_max and
     * (second - best) < this, count as ambiguous; still update only best.
     */
    float match_ambiguity_delta;
    float min_rms_dbfs;         /* NASCENT create energy gate only — not identity */
} ce_config_t;

/** Aggregate lifecycle counters (not per-signature logs). */
typedef struct {
    uint32_t created;
    uint32_t expired;
    uint32_t nascent_evicted;
    uint32_t create_dropped_full;
    uint32_t promote_attempts;
    uint32_t promote_successes;
    uint32_t promotion_enter_events;
    uint32_t promotion_exit_events;
    /** Unadmit after ENTER was not observation-admitted (gen-matched). */
    uint32_t promotion_unadmit;
    /** in_use counts by ae_cand_state_t index 0..5 */
    uint8_t state_hist[6];
    uint8_t in_use_count;
    uint32_t associated;
    uint32_t rejected;
    uint32_t seq_discontinuities;
    uint32_t quality_gaps;
    uint32_t ignored_unusable;
    uint32_t ignored_duplicate;
    uint32_t ignored_low_energy;
    /** Signature near an existing candidate but outside rematch; create suppressed. */
    uint32_t match_hysteresis_hold;
    /** Two+ candidates within match_max and closer than match_ambiguity_delta. */
    uint32_t match_ambiguous;
    uint32_t workflow_completed;
    uint32_t workflow_failed;
    uint32_t module_exec;
    uint32_t module_errors;
    uint32_t module_skipped;
    uint32_t module_missing;
    uint32_t decision_count[CE_DECISION_COUNT];
    /** Slots cleared by ce_stop / ce_manager_release_for_stop (not expire). */
    uint32_t stop_cleared;
    /** Profile match annotations after successful ingest (not promotion). */
    uint32_t profile_match_hits;
    uint32_t profile_match_misses;
} ce_diag_t;

typedef struct {
    bool started;
    ce_config_t cfg;
    ae_candidate_t slots[CE_MAX_CANDIDATES];
    /**
     * Parallel classification annotations (workflow as.profile). Indexed like
     * slots[]; not Stage 1 identity state.
     */
    ce_classification_t classifications[CE_MAX_CANDIDATES];
    uint64_t next_id;
    /**
     * Monotonic CE promotion association generation (never zero after first
     * ENTER). Survives candidate slot clear/reuse so EXIT payloads remain
     * distinguishable across candidate_id reuse. uint32_t wrap skips 0.
     */
    uint32_t next_promotion_gen;
    uint32_t last_processed_seq;
    ce_promotion_cb_t promotion_cb;
    void *promotion_ctx;
    ce_diag_t diag;
    ce_runtime_t runtime;
    const ce_workflow_t *workflow;
    ae_engine_t *bound_ae;
    bool listener_bound;
    ce_decision_t last_decision;
    bool have_decision;
} ce_engine_t;

void ce_config_set_defaults(ce_config_t *cfg);
bool ce_start(ce_engine_t *eng, const ce_config_t *cfg);

/**
 * Safe stop: unbind AE listener, EXIT any promoted lifecycles, clear slots,
 * shut modules down, mark stopped. Idempotent. Does not close Observations.
 */
void ce_stop(ce_engine_t *eng);

/**
 * Register ce_on_signature on ae. A second bind to the same engine replaces
 * the previous binding. Fails if either side is not started.
 */
bool ce_bind_audio_engine(ce_engine_t *eng, ae_engine_t *ae);
void ce_unbind_audio_engine(ce_engine_t *eng);

void ce_set_promotion_cb(ce_engine_t *eng, ce_promotion_cb_t cb, void *ctx);

/** AE listener-compatible entry: void (*)(const ae_signature_t *, void *). */
void ce_on_signature(const ae_signature_t *sig, void *ctx);

/** Direct call when ctx is the engine pointer. */
void ce_engine_on_signature(ce_engine_t *eng, const ae_signature_t *sig);

uint8_t ce_candidate_count(const ce_engine_t *eng);
bool ce_get_candidate(const ce_engine_t *eng, uint8_t index, ae_candidate_t *out);
bool ce_find_candidate(const ce_engine_t *eng, uint64_t id, ae_candidate_t *out);

/**
 * One-time promote ACTIVE/COASTING → PROMOTED and invoke ENTER callback.
 * Second call for the same id returns false. NASCENT, REJECTED, and EXPIRED
 * are declined. This is the integration contract the promotion bridge uses.
 */
bool ce_promote(ce_engine_t *eng, uint64_t candidate_id);

/**
 * Undo an ENTER that was not admitted into an Observation.
 * CE-owned: emits one EXIT when (candidate_id, promotion_gen) matches a live
 * promotion_entered association, restores ACTIVE if currently PROMOTED, and
 * clears promotion_entered. Gen mismatch / already-cleared → no-op (false).
 * Does not reject or expire the candidate.
 */
bool ce_unadmit_promotion(
    ce_engine_t *eng,
    uint64_t candidate_id,
    uint32_t promotion_gen
);

/**
 * Terminal reject. Emits one EXIT if this lifecycle had entered promotion.
 * The slot leaves in_use so a later promote of the same id fails.
 */
bool ce_reject(ce_engine_t *eng, uint64_t candidate_id);

/** NULL restores the built-in track workflow. The table must outlive use. */
void ce_engine_set_workflow(ce_engine_t *eng, const ce_workflow_t *wf);

bool ce_module_register(ce_engine_t *eng, const ce_module_desc_t *desc, void *ctx);

bool ce_module_is_registered(
    const ce_engine_t *eng,
    const char *module_id,
    uint16_t *version_out
);

/** False until a signature or workflow run has recorded a decision. */
bool ce_engine_last_decision(const ce_engine_t *eng, ce_decision_t *out);

/**
 * Latest workflow profile classification for a live candidate.
 * False if the candidate is missing or has no classification annotation yet.
 */
bool ce_get_classification(
    const ce_engine_t *eng,
    uint64_t candidate_id,
    ce_classification_t *out
);

/** Copy counters and refresh state histogram and module totals. */
void ce_get_diag(const ce_engine_t *eng, ce_diag_t *out);

#ifdef __cplusplus
}
#endif
