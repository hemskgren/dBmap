#include "dbmap_episode_registry.h"

#include <string.h>

void dbmap_episode_registry_init(dbmap_episode_registry_t *reg)
{
    if (reg == NULL) {
        return;
    }
    memset(reg, 0, sizeof(*reg));
}

bool dbmap_episode_registry_allocate(
    dbmap_episode_registry_t *reg,
    dbmap_ep_handle_t *out_handle,
    int64_t episode_start_mono_us,
    uint32_t observation_interval_ms,
    uint8_t producer_tag,
    dbmap_ep_uuid_fn uuid_fn,
    bool start_pending
)
{
    if (reg == NULL || out_handle == NULL || uuid_fn == NULL || observation_interval_ms == 0) {
        return false;
    }
    for (uint8_t i = 0; i < DBMAP_OBS_EPISODE_SLOTS; ++i) {
        dbmap_episode_slot_t *s = &reg->slots[i];
        if (s->in_use) {
            continue;
        }
        s->generation = (uint16_t)(s->generation + 1u);
        if (s->generation == 0) {
            s->generation = 1;
        }
        memset(s->observation_id, 0, sizeof(s->observation_id));
        uuid_fn(s->observation_id);
        if (s->observation_id[0] == '\0') {
            return false;
        }
        s->in_use = true;
        s->phase = start_pending ? DBMAP_EP_PENDING : DBMAP_EP_OPEN;
        s->episode_start_mono_us = episode_start_mono_us;
        s->last_report_end_mono_us = episode_start_mono_us;
        s->first_periodic_due_mono_us =
            episode_start_mono_us + ((int64_t)observation_interval_ms * 1000LL);
        s->observation_interval_ms = observation_interval_ms;
        s->start_publish_accepted = false;
        s->pending_term = DBMAP_EP_TERM_NONE;
        s->producer_tag = producer_tag;
        out_handle->index = i;
        out_handle->generation = s->generation;
        return true;
    }
    return false;
}

dbmap_episode_slot_t *dbmap_episode_registry_get(
    dbmap_episode_registry_t *reg,
    dbmap_ep_handle_t handle
)
{
    if (reg == NULL || handle.index >= DBMAP_OBS_EPISODE_SLOTS) {
        return NULL;
    }
    dbmap_episode_slot_t *s = &reg->slots[handle.index];
    if (!s->in_use || s->generation != handle.generation) {
        return NULL;
    }
    return s;
}

const dbmap_episode_slot_t *dbmap_episode_registry_get_const(
    const dbmap_episode_registry_t *reg,
    dbmap_ep_handle_t handle
)
{
    if (reg == NULL || handle.index >= DBMAP_OBS_EPISODE_SLOTS) {
        return NULL;
    }
    const dbmap_episode_slot_t *s = &reg->slots[handle.index];
    if (!s->in_use || s->generation != handle.generation) {
        return NULL;
    }
    return s;
}

void dbmap_episode_registry_mark_start_accepted(
    dbmap_episode_registry_t *reg,
    dbmap_ep_handle_t handle,
    int64_t report_end_mono_us
)
{
    dbmap_episode_slot_t *s = dbmap_episode_registry_get(reg, handle);
    if (s == NULL) {
        return;
    }
    s->start_publish_accepted = true;
    s->phase = DBMAP_EP_OPEN;
    s->last_report_end_mono_us = report_end_mono_us;
}

const char *dbmap_episode_registry_deferred_decision(
    const dbmap_episode_slot_t *slot,
    int64_t now_mono_us,
    bool producer_ended
)
{
    if (slot == NULL || !slot->in_use) {
        return NULL;
    }
    if (slot->start_publish_accepted) {
        return producer_ended ? "end" : NULL;
    }
    if (slot->phase != DBMAP_EP_PENDING && slot->phase != DBMAP_EP_OPEN) {
        return NULL;
    }
    if (producer_ended && now_mono_us < slot->first_periodic_due_mono_us) {
        return "complete";
    }
    if (!producer_ended && now_mono_us >= slot->first_periodic_due_mono_us) {
        return "start";
    }
    if (producer_ended && now_mono_us >= slot->first_periodic_due_mono_us
        && !slot->start_publish_accepted) {
        /* Ended at/after due without ever accepting START: still COMPLETE. */
        return "complete";
    }
    return NULL;
}

void dbmap_episode_registry_mark_open_after_start(
    dbmap_episode_registry_t *reg,
    dbmap_ep_handle_t handle,
    int64_t report_end_mono_us
)
{
    dbmap_episode_registry_mark_start_accepted(reg, handle, report_end_mono_us);
}

void dbmap_episode_registry_note_periodic_published(
    dbmap_episode_registry_t *reg,
    dbmap_ep_handle_t handle,
    int64_t report_end_mono_us
)
{
    dbmap_episode_slot_t *s = dbmap_episode_registry_get(reg, handle);
    if (s == NULL || !s->start_publish_accepted) {
        return;
    }
    s->last_report_end_mono_us = report_end_mono_us;
}

void dbmap_episode_registry_release(
    dbmap_episode_registry_t *reg,
    dbmap_ep_handle_t handle
)
{
    dbmap_episode_slot_t *s = dbmap_episode_registry_get(reg, handle);
    if (s == NULL) {
        return;
    }
    uint16_t gen = s->generation;
    memset(s, 0, sizeof(*s));
    s->generation = gen; /* preserve so stale handles stay invalid */
    s->in_use = false;
    s->phase = DBMAP_EP_FREE;
}

bool dbmap_episode_registry_handle_valid(
    const dbmap_episode_registry_t *reg,
    dbmap_ep_handle_t handle
)
{
    return dbmap_episode_registry_get_const(reg, handle) != NULL;
}

uint8_t dbmap_episode_registry_in_use_count(const dbmap_episode_registry_t *reg)
{
    if (reg == NULL) {
        return 0;
    }
    uint8_t n = 0;
    for (uint8_t i = 0; i < DBMAP_OBS_EPISODE_SLOTS; ++i) {
        if (reg->slots[i].in_use) {
            n++;
        }
    }
    return n;
}
