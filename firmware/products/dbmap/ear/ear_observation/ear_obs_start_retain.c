#include "ear_obs_start_retain.h"

#include <string.h>

typedef struct {
    bool in_use;
    uint16_t generation;
    char observation_id[SP_OBS_EPISODE_ID_LEN];
    ear_obs_start_measure_t measure;
} ear_obs_start_retain_slot_t;

static ear_obs_start_retain_slot_t s_slot[EAR_OBS_START_RETAIN_SLOTS];

static bool same_identity(
    const ear_obs_start_retain_slot_t *slot,
    const char *observation_id,
    uint16_t generation
)
{
    return slot->in_use
        && slot->generation == generation
        && observation_id != NULL
        && strncmp(slot->observation_id, observation_id, SP_OBS_EPISODE_ID_LEN) == 0;
}

void ear_obs_start_retain_reset(void)
{
    memset(s_slot, 0, sizeof(s_slot));
}

bool ear_obs_start_retain_find(
    const char *observation_id,
    uint16_t generation,
    ear_obs_start_measure_t *out
)
{
    if (observation_id == NULL || out == NULL) {
        return false;
    }
    for (uint8_t i = 0u; i < EAR_OBS_START_RETAIN_SLOTS; i++) {
        if (!same_identity(&s_slot[i], observation_id, generation)) {
            continue;
        }
        *out = s_slot[i].measure;
        return true;
    }
    return false;
}

void ear_obs_start_retain_put(
    const char *observation_id,
    uint16_t generation,
    const ear_obs_start_measure_t *in
)
{
    if (observation_id == NULL || observation_id[0] == '\0' || in == NULL) {
        return;
    }
    int free_i = -1;
    for (uint8_t i = 0u; i < EAR_OBS_START_RETAIN_SLOTS; i++) {
        if (same_identity(&s_slot[i], observation_id, generation)) {
            s_slot[i].measure = *in;
            return;
        }
        if (!s_slot[i].in_use && free_i < 0) {
            free_i = (int)i;
        }
    }
    if (free_i < 0) {
        /* Both slots belong to other identities. Caller should have dropped
         * a released binding. Replacing slot 0 keeps the table bounded. */
        free_i = 0;
    }
    memset(&s_slot[free_i], 0, sizeof(s_slot[free_i]));
    s_slot[free_i].in_use = true;
    s_slot[free_i].generation = generation;
    strncpy(
        s_slot[free_i].observation_id,
        observation_id,
        sizeof(s_slot[free_i].observation_id) - 1u
    );
    s_slot[free_i].measure = *in;
}

void ear_obs_start_retain_drop(const char *observation_id, uint16_t generation)
{
    if (observation_id == NULL) {
        return;
    }
    for (uint8_t i = 0u; i < EAR_OBS_START_RETAIN_SLOTS; i++) {
        if (!same_identity(&s_slot[i], observation_id, generation)) {
            continue;
        }
        memset(&s_slot[i], 0, sizeof(s_slot[i]));
    }
}
