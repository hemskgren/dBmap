/**
 * Build and compare acoustic signatures from L0/L1/L2 feature windows.
 * Host-testable; does not touch I2S or PCM.
 */
#pragma once

#include "ae_types.h"

#include "dbmap_acoustic_l0.h"
#include "dbmap_acoustic_l1.h"
#include "dbmap_acoustic_l2.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Build one signature from analyzed feature windows + Platform timing/seq.
 * l1/l2/pair may be NULL (sets missing flags / quality may degrade).
 * Returns false if l0 is NULL or window_seq is 0.
 */
bool ae_signature_build(
    uint32_t window_seq,
    const dbmap_l0_window_t *l0,
    const dbmap_l1_window_t *l1,
    const dbmap_l2_window_t *l2,
    const ae_pair_metrics_t *pair,
    bool gap_before,
    ae_signature_t *out
);

/**
 * Heuristic distance in [0, +inf). Lower = more similar.
 * Used by Candidate Engine association; not a classifier score.
 */
float ae_signature_distance(const ae_signature_t *a, const ae_signature_t *b);

/** True when CE may create/update from this signature. */
bool ae_signature_usable_for_tracking(const ae_signature_t *sig);

#ifdef __cplusplus
}
#endif
