#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dbmap_acoustic_l0.h"
#include "dbmap_acoustic_l1.h"
#include "dbmap_acoustic_l2.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Hierarchical Observation JSON (protocol_version 1).
 *
 * Sections: identity, context, acoustic, spatial.
 * emission_kind: "start" | "periodic" | "end" | "complete"
 * termination_reason required on end/complete; omit on start/periodic.
 */
typedef struct {
    /* identity */
    const char *observation_id;
    const char *node_id;
    const char *hub_id; /* may be empty → still emit string (possibly "") */
    uint32_t sequence_number;

    /* context */
    const char *event_time_utc;
    int64_t capture_timestamp_monotonic_us;
    const char *timing_quality;
    bool has_clock_offset_ms;
    double clock_offset_ms;
    double timestamp_uncertainty_ms;
    double interval_duration_ms;
    double episode_duration_ms;
    const char *emission_kind; /* required */
    /** Required for end/complete; NULL omits (invalid for those kinds). */
    const char *termination_reason;

    /* acoustic */
    bool has_signal_level_dbfs;
    double signal_level_dbfs; /* <= 0 when has_signal_level_dbfs */
    const dbmap_l0_interval_t *measurements; /* required when publishing L0 */
    /**
     * L1 signature: NULL → JSON null (L1 disabled).
     * Non-NULL → schema=l1 object (may be empty: window_count=0, null features).
     */
    const dbmap_l1_interval_t *signature;
    /**
     * L2 dynamics: NULL → JSON null (L2 publish disabled / not provided).
     * Non-NULL → schema=l2 object (may be empty or Option A overflow).
     */
    const dbmap_l2_interval_t *dynamics;
    /* classification always null on physical Ear path */

    /* spatial: bearing/position always null on physical Ear Stage 1 */
} dbmap_observation_fields_t;

/**
 * Build hierarchical Observation JSON (protocol_version 1).
 * Returns heap JSON (caller frees) or NULL on validation error.
 */
char *dbmap_observation_build_json(const dbmap_observation_fields_t *fields);

#ifdef __cplusplus
}
#endif
