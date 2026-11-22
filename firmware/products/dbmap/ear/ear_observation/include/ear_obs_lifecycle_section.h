/**
 * One take / body / give of the activity lifecycle mutex.
 * The body must not take the same mutex. There is no reentry flag.
 */
#pragma once

#include "sp_observation_lifecycle.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*take)(void *lock_ctx);
    void (*give)(void *lock_ctx);
    void *lock_ctx;
} ear_obs_lifecycle_lock_ops_t;

typedef enum {
    EAR_OBS_LIFECYCLE_SECTION_RAN = 0,
    EAR_OBS_LIFECYCLE_SECTION_SKIPPED_GATE = 1,
} ear_obs_lifecycle_section_result_t;

/**
 * take, optionally refuse a closed feed gate, run body, give.
 * gate NULL always runs body (OTA close). A non-NULL gate that does not
 * allow feed gives without calling body.
 * lock NULL runs body with no mutex (startup before the semaphore exists).
 */
ear_obs_lifecycle_section_result_t ear_obs_lifecycle_section(
    const ear_obs_lifecycle_lock_ops_t *lock,
    sp_obs_emit_gate_t *gate,
    void (*body)(void *ctx),
    void *ctx
);

#ifdef __cplusplus
}
#endif
