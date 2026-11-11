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

#define SP_OBS_EPISODE_SLOTS 2
#define SP_OBS_EPISODE_ID_LEN 37

typedef enum {
    SP_EP_FREE = 0,
    SP_EP_PENDING,
    SP_EP_OPEN,
    SP_EP_CLOSING,
} sp_ep_phase_t;

typedef enum {
    SP_EP_TERM_NONE = 0,
    SP_EP_TERM_NATURAL,
    SP_EP_TERM_INVALID_HOLD,
    SP_EP_TERM_OTA_REBOOT,
} sp_ep_termination_t;

/** Opaque handle: slot index + generation to detect stale bindings. */
typedef struct {
    uint8_t index;
    uint16_t generation;
} sp_ep_handle_t;

typedef struct {
    bool in_use;
    sp_ep_phase_t phase;
    uint16_t generation;
    char observation_id[SP_OBS_EPISODE_ID_LEN];
    int64_t episode_start_mono_us;
    int64_t last_report_end_mono_us;
    int64_t first_periodic_due_mono_us;
    uint32_t observation_interval_ms;
    bool start_publish_accepted;
    sp_ep_termination_t pending_term;
    uint8_t producer_tag; /* opaque; 0 = none */
} sp_episode_slot_t;

typedef struct {
    sp_episode_slot_t slots[SP_OBS_EPISODE_SLOTS];
} sp_episode_registry_t;

typedef void (*sp_ep_uuid_fn)(char out[SP_OBS_EPISODE_ID_LEN]);

void sp_episode_registry_init(sp_episode_registry_t *reg);

/**
 * Allocate a free slot. phase starts as PENDING (deferred) or OPEN intent
 * is applied by caller after immediate START path.
 * Returns false if full. Writes handle and observation_id.
 */
bool sp_episode_registry_allocate(
    sp_episode_registry_t *reg,
    sp_ep_handle_t *out_handle,
    int64_t episode_start_mono_us,
    uint32_t observation_interval_ms,
    uint8_t producer_tag,
    sp_ep_uuid_fn uuid_fn,
    bool start_pending
);

sp_episode_slot_t *sp_episode_registry_get(
    sp_episode_registry_t *reg,
    sp_ep_handle_t handle
);

const sp_episode_slot_t *sp_episode_registry_get_const(
    const sp_episode_registry_t *reg,
    sp_ep_handle_t handle
);

/** Mark START accepted (MQTT client enqueue success). */
void sp_episode_registry_mark_start_accepted(
    sp_episode_registry_t *reg,
    sp_ep_handle_t handle,
    int64_t report_end_mono_us
);

/**
 * Deferred decision at close or at first_periodic_due:
 * - ended before due + no START accepted → kind "complete"
 * - still active at due → kind "start" (caller publishes START)
 * Returns emission kind string pointer into static literals, or NULL if invalid.
 */
const char *sp_episode_registry_deferred_decision(
    const sp_episode_slot_t *slot,
    int64_t now_mono_us,
    bool producer_ended
);

void sp_episode_registry_mark_open_after_start(
    sp_episode_registry_t *reg,
    sp_ep_handle_t handle,
    int64_t report_end_mono_us
);

void sp_episode_registry_note_periodic_published(
    sp_episode_registry_t *reg,
    sp_ep_handle_t handle,
    int64_t report_end_mono_us
);

/** Release slot; observation_id must never be reused. */
void sp_episode_registry_release(
    sp_episode_registry_t *reg,
    sp_ep_handle_t handle
);

bool sp_episode_registry_handle_valid(
    const sp_episode_registry_t *reg,
    sp_ep_handle_t handle
);

uint8_t sp_episode_registry_in_use_count(const sp_episode_registry_t *reg);

#ifdef __cplusplus
}
#endif
