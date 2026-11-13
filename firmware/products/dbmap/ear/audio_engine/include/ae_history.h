#pragma once

#include "ae_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AE_HISTORY_CAPACITY
#define AE_HISTORY_CAPACITY 32u
#endif

typedef struct {
    ae_signature_t entries[AE_HISTORY_CAPACITY];
    uint32_t count; /* min(pushed, capacity) */
    uint32_t next;  /* next write index */
    uint32_t pushed;
} ae_history_t;

void ae_history_init(ae_history_t *hist);
void ae_history_push(ae_history_t *hist, const ae_signature_t *sig);
bool ae_history_latest(const ae_history_t *hist, ae_signature_t *out);

/** Copy up to max_n newest-first signatures. Returns number copied. */
uint32_t ae_history_copy_newest(
    const ae_history_t *hist,
    ae_signature_t *out,
    uint32_t max_n
);

/**
 * Copy up to max_n newest-first signatures with
 * ``window_end_mono_us >= min_end_mono_us`` (inclusive).
 * Ordering among the filtered set remains newest-first.
 */
uint32_t ae_history_copy_since_mono(
    const ae_history_t *hist,
    int64_t min_end_mono_us,
    ae_signature_t *out,
    uint32_t max_n
);

/**
 * Drop retained entries with ``window_end_mono_us < min_end_mono_us``.
 * Preserves relative newest order of survivors. Returns number removed.
 */
uint32_t ae_history_expire_before(ae_history_t *hist, int64_t min_end_mono_us);

#ifdef __cplusplus
}
#endif
