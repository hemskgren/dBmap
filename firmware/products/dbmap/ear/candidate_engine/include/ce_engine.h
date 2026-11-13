/**
 * Candidate Engine v1 — multi-candidate lifecycle over ae_signature_t.
 * Engine-to-engine consumer of AE; not Platform transport; not MQTT.
 */
#pragma once

#include "ce_types.h"

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
    float match_distance_max;   /* associate if distance <= this */
    float min_rms_dbfs;         /* ignore quieter than this for create */
} ce_config_t;

/** Aggregate lifecycle counters (not per-signature). */
typedef struct {
    uint32_t created;
    uint32_t expired;
    uint32_t nascent_evicted;
    uint32_t create_dropped_full;
    uint32_t promote_attempts;
    uint32_t promote_successes;
    uint32_t promotion_enter_events;
    uint32_t promotion_exit_events;
    /** in_use counts by ae_cand_state_t index 0..5 */
    uint8_t state_hist[6];
    uint8_t in_use_count;
} ce_diag_t;

typedef struct {
    bool started;
    ce_config_t cfg;
    ae_candidate_t slots[CE_MAX_CANDIDATES];
    uint64_t next_id;
    uint32_t last_processed_seq;
    ce_promotion_cb_t promotion_cb;
    void *promotion_ctx;
    ce_diag_t diag;
} ce_engine_t;

void ce_config_set_defaults(ce_config_t *cfg);
bool ce_start(ce_engine_t *eng, const ce_config_t *cfg);
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
 * Second call for the same id returns false.
 */
bool ce_promote(ce_engine_t *eng, uint64_t candidate_id);

/** Copy counters and refresh state histogram from current slots. */
void ce_get_diag(const ce_engine_t *eng, ce_diag_t *out);

#ifdef __cplusplus
}
#endif
