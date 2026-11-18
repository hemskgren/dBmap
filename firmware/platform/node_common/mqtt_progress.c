#include "sp_mqtt_progress.h"

#include <stddef.h>

static void clear_slots(sp_mqtt_progress_t *state)
{
    for (uint32_t i = 0; i < SP_MQTT_PROGRESS_SLOTS; ++i) {
        state->slots[i].used = false;
        state->slots[i].msg_id = 0;
        state->slots[i].seq = 0;
    }
}

static void clear_early(sp_mqtt_progress_t *state)
{
    for (uint32_t i = 0; i < SP_MQTT_PROGRESS_EARLY_SLOTS; ++i) {
        state->early_acks[i] = 0;
    }
    state->early_count = 0;
}

static void mark_verified(sp_mqtt_progress_t *state, int64_t now_ms)
{
    state->last_verified_ms = now_ms;
    if (state->verified_count < UINT32_MAX) {
        state->verified_count++;
    }
    state->deadline_ms = now_ms + (int64_t)SP_MQTT_PROGRESS_LIMIT_MS;
}

static bool take_early(sp_mqtt_progress_t *state, int msg_id)
{
    for (uint32_t i = 0; i < SP_MQTT_PROGRESS_EARLY_SLOTS; ++i) {
        if (state->early_acks[i] != msg_id) {
            continue;
        }
        state->early_acks[i] = 0;
        if (state->early_count > 0) {
            state->early_count--;
        }
        return true;
    }
    return false;
}

static bool park_early(sp_mqtt_progress_t *state, int msg_id)
{
    uint32_t free_slot = SP_MQTT_PROGRESS_EARLY_SLOTS;
    for (uint32_t i = 0; i < SP_MQTT_PROGRESS_EARLY_SLOTS; ++i) {
        if (state->early_acks[i] == msg_id) {
            return true;
        }
        if (state->early_acks[i] == 0 && free_slot == SP_MQTT_PROGRESS_EARLY_SLOTS) {
            free_slot = i;
        }
    }
    if (free_slot == SP_MQTT_PROGRESS_EARLY_SLOTS) {
        /* Drop oldest park (index 0) and shift — bounded memory. */
        for (uint32_t i = 1; i < SP_MQTT_PROGRESS_EARLY_SLOTS; ++i) {
            state->early_acks[i - 1] = state->early_acks[i];
        }
        free_slot = SP_MQTT_PROGRESS_EARLY_SLOTS - 1;
        state->early_acks[free_slot] = 0;
        if (state->early_count > 0) {
            state->early_count--;
        }
    }
    state->early_acks[free_slot] = msg_id;
    if (state->early_count < SP_MQTT_PROGRESS_EARLY_SLOTS) {
        state->early_count++;
    }
    return true;
}

void sp_mqtt_progress_init(sp_mqtt_progress_t *state)
{
    if (state == NULL) {
        return;
    }
    clear_slots(state);
    clear_early(state);
    state->next_seq = 1;
    state->connected = false;
    state->last_verified_ms = 0;
    state->verified_count = 0;
    state->deadline_ms = 0;
}

void sp_mqtt_progress_on_connected(sp_mqtt_progress_t *state, int64_t now_ms)
{
    if (state == NULL) {
        return;
    }
    clear_slots(state);
    clear_early(state);
    state->connected = true;
    state->deadline_ms = now_ms + (int64_t)SP_MQTT_PROGRESS_LIMIT_MS;
}

void sp_mqtt_progress_on_disconnected(sp_mqtt_progress_t *state)
{
    if (state == NULL) {
        return;
    }
    clear_slots(state);
    clear_early(state);
    state->connected = false;
    state->deadline_ms = 0;
}

sp_mqtt_progress_note_result_t sp_mqtt_progress_note_qos1(
    sp_mqtt_progress_t *state,
    int msg_id,
    int64_t now_ms
)
{
    uint32_t free_slot = SP_MQTT_PROGRESS_SLOTS;
    uint32_t oldest = 0;
    uint16_t oldest_seq = UINT16_MAX;

    if (state == NULL || msg_id <= 0) {
        return SP_MQTT_PROGRESS_NOTE_REJECTED;
    }
    if (take_early(state, msg_id)) {
        mark_verified(state, now_ms);
        return SP_MQTT_PROGRESS_NOTE_VERIFIED_EARLY;
    }
    for (uint32_t i = 0; i < SP_MQTT_PROGRESS_SLOTS; ++i) {
        if (state->slots[i].used && state->slots[i].msg_id == msg_id) {
            return SP_MQTT_PROGRESS_NOTE_TRACKED;
        }
        if (!state->slots[i].used && free_slot == SP_MQTT_PROGRESS_SLOTS) {
            free_slot = i;
        }
        if (state->slots[i].used && state->slots[i].seq < oldest_seq) {
            oldest_seq = state->slots[i].seq;
            oldest = i;
        }
    }
    if (free_slot == SP_MQTT_PROGRESS_SLOTS) {
        free_slot = oldest;
    }
    state->slots[free_slot].used = true;
    state->slots[free_slot].msg_id = msg_id;
    state->slots[free_slot].seq = state->next_seq;
    if (state->next_seq < UINT16_MAX) {
        state->next_seq++;
    } else {
        state->next_seq = 1;
    }
    return SP_MQTT_PROGRESS_NOTE_TRACKED;
}

void sp_mqtt_progress_note_qos0(sp_mqtt_progress_t *state)
{
    (void)state;
}

sp_mqtt_progress_pub_result_t sp_mqtt_progress_on_published(
    sp_mqtt_progress_t *state,
    int msg_id,
    int64_t now_ms
)
{
    if (state == NULL || !state->connected || msg_id <= 0) {
        return SP_MQTT_PROGRESS_PUB_IGNORED;
    }
    for (uint32_t i = 0; i < SP_MQTT_PROGRESS_SLOTS; ++i) {
        if (!state->slots[i].used || state->slots[i].msg_id != msg_id) {
            continue;
        }
        state->slots[i].used = false;
        state->slots[i].msg_id = 0;
        mark_verified(state, now_ms);
        return SP_MQTT_PROGRESS_PUB_VERIFIED;
    }
    (void)park_early(state, msg_id);
    return SP_MQTT_PROGRESS_PUB_PARKED_EARLY;
}

void sp_mqtt_progress_on_inbound_data(sp_mqtt_progress_t *state, int64_t now_ms)
{
    if (state == NULL || !state->connected) {
        return;
    }
    mark_verified(state, now_ms);
}

bool sp_mqtt_progress_expired(const sp_mqtt_progress_t *state, int64_t now_ms)
{
    if (state == NULL || !state->connected || state->deadline_ms == 0) {
        return false;
    }
    return now_ms >= state->deadline_ms;
}

int64_t sp_mqtt_progress_age_ms(const sp_mqtt_progress_t *state, int64_t now_ms)
{
    if (state == NULL || !state->connected) {
        return -1;
    }
    if (state->last_verified_ms == 0) {
        /* Connected but no verified delivery yet: age from deadline start. */
        int64_t start = state->deadline_ms - (int64_t)SP_MQTT_PROGRESS_LIMIT_MS;
        if (start <= 0) {
            return now_ms;
        }
        return now_ms - start;
    }
    return now_ms - state->last_verified_ms;
}

int64_t sp_mqtt_progress_last_verified_ms(const sp_mqtt_progress_t *state)
{
    if (state == NULL) {
        return 0;
    }
    return state->last_verified_ms;
}
