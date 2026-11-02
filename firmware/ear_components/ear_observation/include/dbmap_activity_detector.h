#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * RMS acoustic activity detector (host-testable).
 *
 * Detects one aggregate activity stream per Ear. Does not classify sources,
 * estimate bearing, or separate simultaneous sounds.
 *
 * Input-quality gates (transport + reject hard mic faults) decide whether an
 * RMS sample may drive the detector. Activity itself is decided only by RMS
 * enter/exit thresholds — not by microphone_status == signal_present.
 *
 * Detection evaluation is independent of MQTT / Observation emission success.
 */

typedef enum {
    DBMAP_ACTIVITY_IDLE = 0,
    DBMAP_ACTIVITY_PENDING_ENTER,
    DBMAP_ACTIVITY_ACTIVE,
    DBMAP_ACTIVITY_PENDING_EXIT,
} dbmap_activity_state_t;

typedef enum {
    DBMAP_ACTIVITY_EMIT_NONE = 0,
    DBMAP_ACTIVITY_EMIT_START,
    DBMAP_ACTIVITY_EMIT_PERIODIC,
    DBMAP_ACTIVITY_EMIT_END,
} dbmap_activity_emit_kind_t;

typedef struct {
    /** Enter when rms_dbfs >= enter_threshold_dbfs (provisional, not calibrated). */
    float enter_threshold_dbfs;
    /** Exit when rms_dbfs < exit_threshold_dbfs; must be < enter_threshold_dbfs. */
    float exit_threshold_dbfs;
    uint32_t min_activity_ms;
    uint32_t exit_hold_ms;
    uint32_t boot_settle_ms;
    uint32_t observation_interval_ms;
    /**
     * While ACTIVE / PENDING_*: if no usable sample for this long, force END
     * (invalid/missing is never treated as silence, but episodes must not linger
     * forever). 0 disables the forced end (not recommended).
     */
    uint32_t max_invalid_hold_ms;
} dbmap_activity_config_t;

typedef struct {
    bool has_measurement;
    float rms_dbfs;
    uint32_t window_ms;
    int64_t measured_at_us;
    uint32_t window_seq;
    uint32_t ms_since_i2s_enable;
    /** I2S fill health: ok | degraded | error | unavailable */
    const char *transport_status;
    /**
     * Mic presence diagnostic from dbmap_audio_classify_microphone — not
     * source_family. Used only to reject hard faults (see sample_usable).
     */
    const char *microphone_status;
} dbmap_activity_sample_t;

typedef struct {
    dbmap_activity_emit_kind_t kind;
    float signal_level_dbfs;
    double duration_ms;
    int64_t interval_start_mono_us;
    int64_t interval_end_mono_us;
} dbmap_activity_emission_t;

typedef struct {
    dbmap_activity_config_t cfg;
    dbmap_activity_state_t state;
    bool boot_complete;
    int64_t pending_since_us;
    int64_t episode_start_us;
    int64_t last_emit_us;
    int64_t last_valid_us;
    float last_level_dbfs;
    uint32_t last_window_seq;
    bool has_last_window;
    bool has_last_valid;
    /* Statistics for tests / diagnostics */
    uint32_t windows_seen;
    uint32_t windows_invalid;
    uint32_t windows_boot_settle;
    uint32_t emissions_start;
    uint32_t emissions_periodic;
    uint32_t emissions_end;
    uint32_t forced_ends_invalid;
} dbmap_activity_detector_t;

/** Validate config; returns false if thresholds/intervals are inconsistent. */
bool dbmap_activity_config_valid(const dbmap_activity_config_t *cfg);

void dbmap_activity_detector_init(dbmap_activity_detector_t *det, const dbmap_activity_config_t *cfg);

/**
 * Input-quality gate (not activity detection).
 *
 * Requires: finite rms_dbfs <= 0, transport ok|degraded, and microphone_status
 * not a hard fault (unavailable / disconnected_suspected / noise_suspected).
 * Allows signal_present and unknown so weak but valid RMS is not filtered out
 * before the detector's own thresholds run.
 */
bool dbmap_activity_sample_usable(const dbmap_activity_sample_t *sample);

/**
 * Feed one completed measurement window.
 *
 * Ignores duplicate window_seq. Skips boot-settle and unusable samples without
 * treating them as acoustic quiet. May return at most one emission per call.
 * MQTT must not gate this function.
 */
dbmap_activity_emission_t dbmap_activity_detector_feed(
    dbmap_activity_detector_t *det,
    const dbmap_activity_sample_t *sample
);

dbmap_activity_state_t dbmap_activity_detector_state(const dbmap_activity_detector_t *det);

#ifdef __cplusplus
}
#endif
