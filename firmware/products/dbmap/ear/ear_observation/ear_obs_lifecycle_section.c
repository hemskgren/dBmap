#include "ear_obs_lifecycle_section.h"

#include <stddef.h>

ear_obs_lifecycle_section_result_t ear_obs_lifecycle_section(
    const ear_obs_lifecycle_lock_ops_t *lock,
    sp_obs_emit_gate_t *gate,
    void (*body)(void *ctx),
    void *ctx
)
{
    const bool locked = (lock != NULL && lock->take != NULL && lock->give != NULL);
    if (locked) {
        lock->take(lock->lock_ctx);
    }
    if (gate != NULL && !sp_obs_gate_allows_feed(*gate)) {
        if (locked) {
            lock->give(lock->lock_ctx);
        }
        return EAR_OBS_LIFECYCLE_SECTION_SKIPPED_GATE;
    }
    if (body != NULL) {
        body(ctx);
    }
    if (locked) {
        lock->give(lock->lock_ctx);
    }
    return EAR_OBS_LIFECYCLE_SECTION_RAN;
}
