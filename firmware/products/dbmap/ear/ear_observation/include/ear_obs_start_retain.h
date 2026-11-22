/**
 * Fixed two-slot retain of the START measurement aggregates.
 * The publisher owns this. It is not an MQTT outbox and not PCM history.
 */
#pragma once

#include "dbmap_acoustic_l0.h"
#include "dbmap_acoustic_l1.h"
#include "dbmap_acoustic_l2.h"
#include "sp_episode_registry.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EAR_OBS_START_RETAIN_SLOTS 2u

typedef struct {
    int64_t capture_mono_us;
    int64_t interval_start_mono_us;
    dbmap_l0_interval_t l0;
    bool have_l1;
    dbmap_l1_interval_t l1;
    bool have_l2;
    dbmap_l2_interval_t l2;
} ear_obs_start_measure_t;

void ear_obs_start_retain_reset(void);

bool ear_obs_start_retain_find(
    const char *observation_id,
    uint16_t generation,
    ear_obs_start_measure_t *out
);

void ear_obs_start_retain_put(
    const char *observation_id,
    uint16_t generation,
    const ear_obs_start_measure_t *in
);

void ear_obs_start_retain_drop(const char *observation_id, uint16_t generation);

#ifdef __cplusplus
}
#endif
