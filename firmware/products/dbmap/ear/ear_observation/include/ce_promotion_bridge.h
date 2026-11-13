/**
 * M3 Candidate Promotion Bridge (Ear product).
 *
 * Association-only: links a promoted CE candidate to the sole open RMS
 * Observation episode. Never opens or ends Observations.
 *
 * Host-testable; FreeRTOS/MQTT-free.
 *
 * Device concurrency invariant (current firmware):
 * All production CE promotion callbacks and Bridge queue operations currently
 * execute on activity_task. EXIT may be enqueued during AE/CE polling;
 * ENTER is enqueued synchronously during ce_bridge_poll(). The queue is
 * therefore single-task serialized in the current firmware. Any future
 * additional callback producer or task must introduce appropriate
 * synchronization and revisit this assumption.
 */
#pragma once

#include "ce_engine.h"
#include "sp_episode_registry.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CE_BRIDGE_QUEUE_CAP
#define CE_BRIDGE_QUEUE_CAP 4u
#endif

typedef struct {
    uint32_t queue_overflows;
    uint32_t enter_queued;
    uint32_t exit_queued;
    uint32_t assoc_success;
    uint32_t assoc_fail_no_obs;
    uint32_t assoc_fail_stale_handle;
    uint32_t assoc_reject_overwrite;
    uint32_t enter_idempotent;
    uint32_t disassoc_on_exit;
    uint32_t disassoc_on_obs_end;
    uint32_t promote_calls;
    uint32_t promote_skipped;
} ce_bridge_diag_t;

/**
 * Immutable copy of counters + current association for UART/host diagnostics.
 *
 * Consistency: under the current single-task activity_task model, a snapshot
 * taken outside of queue push/drain is coherent with bridge state at call time.
 * Do not retain pointers into bridge internals; IDs are copied by value.
 * When bound==false, candidate_id is 0 and observation_id is empty (not stale).
 */
typedef struct {
    ce_bridge_diag_t diag;
    bool bound;
    uint64_t candidate_id;
    char observation_id[SP_OBS_EPISODE_ID_LEN];
} ce_bridge_diag_snapshot_t;

typedef struct {
    uint64_t candidate_id;
    ce_promotion_event_t ev;
} ce_bridge_event_t;

typedef struct {
    bool bound;
    uint64_t candidate_id;
    char observation_id[SP_OBS_EPISODE_ID_LEN];
    sp_ep_handle_t ep_handle;
} ce_bridge_assoc_t;

typedef struct {
    ce_engine_t *ce;
    bool obs_context_valid;
    char obs_id[SP_OBS_EPISODE_ID_LEN];
    sp_ep_handle_t obs_handle;

    ce_bridge_assoc_t assoc;

    ce_bridge_event_t queue[CE_BRIDGE_QUEUE_CAP];
    uint8_t q_head;
    uint8_t q_count;

    ce_bridge_diag_t diag;
} ce_promotion_bridge_t;

void ce_bridge_init(ce_promotion_bridge_t *bridge);
void ce_bridge_bind_engine(ce_promotion_bridge_t *bridge, ce_engine_t *ce);

/**
 * CE promotion callback — enqueue only (non-blocking).
 * Register via ce_set_promotion_cb(ce, ce_bridge_on_promotion, bridge).
 */
void ce_bridge_on_promotion(
    const ae_candidate_t *c,
    ce_promotion_event_t ev,
    void *ctx
);

/** RMS episode allocated / open — association target. */
void ce_bridge_notify_obs_open(
    ce_promotion_bridge_t *bridge,
    const char *observation_id,
    sp_ep_handle_t handle
);

/** RMS episode released / ended — clear association. */
void ce_bridge_notify_obs_closed(ce_promotion_bridge_t *bridge);

/**
 * Drain deferred ENTER/EXIT, then ensure association for the current
 * Observation context when available (D1/D4):
 *
 * 1. If unbound and a live promoted lifecycle exists (PROMOTED or
 *    promotion_entered, including COASTING), re-associate that candidate to
 *    the current observation_id / episode handle without ce_promote / ENTER.
 * 2. Else select the primary ACTIVE candidate, promote (ENTER), and associate.
 *
 * Observation close clears association only; it does not EXIT the CE lifecycle.
 *
 * registry may be NULL in host tests that only exercise queue/assoc without
 * generation checks; when non-NULL, stored handles are validated before bind.
 */
void ce_bridge_poll(
    ce_promotion_bridge_t *bridge,
    const sp_episode_registry_t *registry
);

/**
 * Deterministic primary selection among ACTIVE candidates (D4).
 * Returns false if none eligible, or if any in-use candidate is PROMOTED or
 * still has promotion_entered (e.g. COASTING after ENTER before rematch).
 */
bool ce_bridge_select_primary(const ce_engine_t *ce, uint64_t *out_candidate_id);

bool ce_bridge_is_bound(const ce_promotion_bridge_t *bridge);
uint64_t ce_bridge_bound_candidate_id(const ce_promotion_bridge_t *bridge);
const char *ce_bridge_bound_observation_id(const ce_promotion_bridge_t *bridge);

void ce_bridge_get_diag(const ce_promotion_bridge_t *bridge, ce_bridge_diag_t *out);

/** Copy counters and current association (see ce_bridge_diag_snapshot_t). */
void ce_bridge_get_diag_snapshot(
    const ce_promotion_bridge_t *bridge,
    ce_bridge_diag_snapshot_t *out
);

#ifdef __cplusplus
}
#endif
