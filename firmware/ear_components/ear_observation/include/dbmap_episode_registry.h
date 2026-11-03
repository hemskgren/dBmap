#pragma once

/**
 * Stage 1 — bounded concurrent Observation episode registry (host-testable).
 *
 * Capacity: 2 interchangeable slots (no semantic slot index).
 * observation_id is minted once at allocate and never reused after release.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBMAP_OBS_EPISODE_SLOTS 2
#define DBMAP_OBS_EPISODE_ID_LEN 37

typedef enum {
    DBMAP_EP_FREE = 0,
    DBMAP_EP_PENDING,
    DBMAP_EP_OPEN,
    DBMAP_EP_CLOSING,
} dbmap_ep_phase_t;

typedef enum {
    DBMAP_EP_TERM_NONE = 0,
    DBMAP_EP_TERM_NATURAL,
    DBMAP_EP_TERM_INVALID_HOLD,
    DBMAP_EP_TERM_OTA_REBOOT,
} dbmap_ep_termination_t;

/** Opaque handle: slot index + generation to detect stale bindings. */
typedef struct {
    uint8_t index;
    uint16_t generation;
} dbmap_ep_handle_t;

typedef struct {
    bool in_use;
    dbmap_ep_phase_t phase;
    uint16_t generation;
    char observation_id[DBMAP_OBS_EPISODE_ID_LEN];
    int64_t episode_start_mono_us;
    int64_t last_report_end_mono_us;
    int64_t first_periodic_due_mono_us;
    uint32_t observation_interval_ms;
    bool start_publish_accepted;
    dbmap_ep_termination_t pending_term;
    uint8_t producer_tag; /* opaque; 0 = none */
} dbmap_episode_slot_t;

typedef struct {
    dbmap_episode_slot_t slots[DBMAP_OBS_EPISODE_SLOTS];
} dbmap_episode_registry_t;

typedef void (*dbmap_ep_uuid_fn)(char out[DBMAP_OBS_EPISODE_ID_LEN]);

void dbmap_episode_registry_init(dbmap_episode_registry_t *reg);

/**
 * Allocate a free slot. phase starts as PENDING (deferred) or OPEN intent
 * is applied by caller after immediate START path.
 * Returns false if full. Writes handle and observation_id.
 */
bool dbmap_episode_registry_allocate(
    dbmap_episode_registry_t *reg,
    dbmap_ep_handle_t *out_handle,
    int64_t episode_start_mono_us,
    uint32_t observation_interval_ms,
    uint8_t producer_tag,
    dbmap_ep_uuid_fn uuid_fn,
    bool start_pending
);

dbmap_episode_slot_t *dbmap_episode_registry_get(
    dbmap_episode_registry_t *reg,
    dbmap_ep_handle_t handle
);

const dbmap_episode_slot_t *dbmap_episode_registry_get_const(
    const dbmap_episode_registry_t *reg,
    dbmap_ep_handle_t handle
);

/** Mark START accepted (MQTT client enqueue success). */
void dbmap_episode_registry_mark_start_accepted(
    dbmap_episode_registry_t *reg,
    dbmap_ep_handle_t handle,
    int64_t report_end_mono_us
);

/**
 * Deferred decision at close or at first_periodic_due:
 * - ended before due + no START accepted → kind "complete"
 * - still active at due → kind "start" (caller publishes START)
 * Returns emission kind string pointer into static literals, or NULL if invalid.
 */
const char *dbmap_episode_registry_deferred_decision(
    const dbmap_episode_slot_t *slot,
    int64_t now_mono_us,
    bool producer_ended
);

void dbmap_episode_registry_mark_open_after_start(
    dbmap_episode_registry_t *reg,
    dbmap_ep_handle_t handle,
    int64_t report_end_mono_us
);

void dbmap_episode_registry_note_periodic_published(
    dbmap_episode_registry_t *reg,
    dbmap_ep_handle_t handle,
    int64_t report_end_mono_us
);

/** Release slot; observation_id must never be reused. */
void dbmap_episode_registry_release(
    dbmap_episode_registry_t *reg,
    dbmap_ep_handle_t handle
);

bool dbmap_episode_registry_handle_valid(
    const dbmap_episode_registry_t *reg,
    dbmap_ep_handle_t handle
);

uint8_t dbmap_episode_registry_in_use_count(const dbmap_episode_registry_t *reg);

#ifdef __cplusplus
}
#endif
