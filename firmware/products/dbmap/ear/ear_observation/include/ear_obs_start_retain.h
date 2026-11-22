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

/**
 * Insert or replace this identity. Returns false when the arguments are
 * unusable or both slots belong to other identities. A false return leaves
 * every existing entry unchanged.
 */
bool ear_obs_start_retain_put(
    const char *observation_id,
    uint16_t generation,
    const ear_obs_start_measure_t *in
);

typedef struct {
    dbmap_l0_interval_t l0;
    bool have_l1;
    dbmap_l1_interval_t l1;
    bool have_l2;
    dbmap_l2_interval_t l2;
} ear_obs_start_aggregates_t;

typedef void (*ear_obs_start_build_fn)(
    void *ctx,
    int64_t interval_start_mono_us,
    int64_t capture_mono_us,
    ear_obs_start_aggregates_t *out
);

typedef enum {
    EAR_OBS_START_PREPARE_READY = 0,
    EAR_OBS_START_PREPARE_RETAIN_FULL = 1,
    EAR_OBS_START_PREPARE_BAD_ARG = 2,
} ear_obs_start_prepare_status_t;

/**
 * Production START preparation used by mgr_emit_start.
 * A hit reuses the retained aggregate and does not call build.
 * A miss builds once and retains the result. A full table does not build
 * and does not publish: out is left untouched.
 * built_this_call may be NULL.
 */
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
);

void ear_obs_start_retain_drop(const char *observation_id, uint16_t generation);

#ifdef __cplusplus
}
#endif
