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

bool ear_obs_start_retain_put(
    const char *observation_id,
    uint16_t generation,
    const ear_obs_start_measure_t *in
)
{
    if (observation_id == NULL || observation_id[0] == '\0' || in == NULL) {
        return false;
    }
    int free_i = -1;
    for (uint8_t i = 0u; i < EAR_OBS_START_RETAIN_SLOTS; i++) {
        if (same_identity(&s_slot[i], observation_id, generation)) {
            s_slot[i].measure = *in;
            return true;
        }
        if (!s_slot[i].in_use && free_i < 0) {
            free_i = (int)i;
        }
    }
    if (free_i < 0) {
        return false;
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
    return true;
}

static bool retain_has_identity_or_room(const char *observation_id, uint16_t generation)
{
    bool have_free = false;
    for (uint8_t i = 0u; i < EAR_OBS_START_RETAIN_SLOTS; i++) {
        if (same_identity(&s_slot[i], observation_id, generation)) {
            return true;
        }
        if (!s_slot[i].in_use) {
            have_free = true;
        }
    }
    return have_free;
}

ear_obs_start_prepare_status_t ear_obs_start_prepare(
    const char *observation_id,
    uint16_t generation,
    int64_t capture_mono_us,
    int64_t binding_start_mono_us,
    int64_t interval_us,
    ear_obs_start_build_fn build,
    void *build_ctx,
    ear_obs_start_measure_t *out,
    bool *built_this_call
)
{
    if (built_this_call != NULL) {
        *built_this_call = false;
    }
    if (observation_id == NULL || observation_id[0] == '\0' || out == NULL || build == NULL) {
        return EAR_OBS_START_PREPARE_BAD_ARG;
    }
    if (ear_obs_start_retain_find(observation_id, generation, out)) {
        return EAR_OBS_START_PREPARE_READY;
    }
    if (!retain_has_identity_or_room(observation_id, generation)) {
        return EAR_OBS_START_PREPARE_RETAIN_FULL;
    }

    int64_t iv_start = capture_mono_us - interval_us;
    if (iv_start < binding_start_mono_us) {
        iv_start = binding_start_mono_us;
    }
    ear_obs_start_aggregates_t built;
    memset(&built, 0, sizeof(built));
    build(build_ctx, iv_start, capture_mono_us, &built);

    ear_obs_start_measure_t meas;
    memset(&meas, 0, sizeof(meas));
    meas.capture_mono_us = capture_mono_us;
    meas.interval_start_mono_us = iv_start;
    meas.l0 = built.l0;
    meas.have_l1 = built.have_l1;
    if (built.have_l1) {
        meas.l1 = built.l1;
    }
    meas.have_l2 = built.have_l2;
    if (built.have_l2) {
        meas.l2 = built.l2;
    }
    if (!ear_obs_start_retain_put(observation_id, generation, &meas)) {
        return EAR_OBS_START_PREPARE_RETAIN_FULL;
    }
    *out = meas;
    if (built_this_call != NULL) {
        *built_this_call = true;
    }
    return EAR_OBS_START_PREPARE_READY;
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
