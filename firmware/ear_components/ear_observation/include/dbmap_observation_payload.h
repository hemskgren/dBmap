#pragma once

#include <stdint.h>

#include "dbmap_acoustic_l0.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Observation JSON fields (protocol v1, optionally extended with L0).
 *
 * protocol_version is always 1.
 * Legacy: emission_kind == NULL && measurements == NULL.
 * Extended: both emission_kind and measurements non-NULL (required together).
 */
typedef struct {
    const char *observation_id; /* UUID string */
    const char *node_id;
    uint32_t sequence_number;
    const char *event_time_utc; /* ISO-8601 with timezone, typically …Z */
    int64_t capture_timestamp_monotonic_us;
    const char *timing_quality; /* synchronized | estimated | unsynchronized */
    double timestamp_uncertainty_ms;
    double signal_level_dbfs; /* <= 0; detector closing-window RMS (digital FS) */
    double duration_ms;       /* measured interval length; >= 0 */

    /** Extended: "start" | "periodic" | "end" (NULL = legacy payload). */
    const char *emission_kind;
    /**
     * Optional on END only: "natural" | "invalid_hold" | "ota_reboot".
     * NULL omits the field (legacy END). Rejected by builder on non-END.
     */
    const char *termination_reason;
    /** Extended: finalized L0 aggregate (NULL = legacy). Required with emission_kind. */
    const dbmap_l0_interval_t *measurements;
} dbmap_observation_fields_t;

/**
 * Build unformatted Observation JSON (protocol_version 1).
 * Always emits classification and bearing as JSON null (no simulator hints).
 * Returns heap JSON (caller frees) or NULL on error / incomplete extension pair.
 */
char *dbmap_observation_build_json(const dbmap_observation_fields_t *fields);

#ifdef __cplusplus
}
#endif
