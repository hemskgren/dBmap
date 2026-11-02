#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fields required to serialize one protocol-v1 Observation (no classification/bearing). */
typedef struct {
    const char *observation_id; /* UUID string */
    const char *node_id;
    uint32_t sequence_number;
    const char *event_time_utc; /* ISO-8601 with timezone, typically …Z */
    int64_t capture_timestamp_monotonic_us;
    const char *timing_quality; /* synchronized | estimated | unsynchronized */
    double timestamp_uncertainty_ms;
    double signal_level_dbfs; /* <= 0; digital full-scale, not SPL */
    double duration_ms;       /* measured interval length; >= 0 */
} dbmap_observation_fields_t;

/**
 * Build unformatted Observation JSON.
 * Always emits classification and bearing as JSON null (no simulator hints).
 * Returns heap JSON (caller frees) or NULL on error.
 */
char *dbmap_observation_build_json(const dbmap_observation_fields_t *fields);

#ifdef __cplusplus
}
#endif
