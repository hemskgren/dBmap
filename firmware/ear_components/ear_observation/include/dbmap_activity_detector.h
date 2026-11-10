#pragma once

#include <stdbool.h>
#include <stddef.h>
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
 * Optional Relative END (v1): freeze a pre-event RMS baseline at START and end
 * when level returns near that baseline, while keeping absolute exit as fallback.
 * When relative_end_enabled is false, behavior matches the absolute-only detector.
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

/** Meaningful on END emissions only (OTA reason is applied by the Observation layer). */
typedef enum {
    DBMAP_ACTIVITY_TERM_NONE = 0,
    DBMAP_ACTIVITY_TERM_NATURAL,
    DBMAP_ACTIVITY_TERM_INVALID_HOLD,
} dbmap_activity_termination_t;

/** Latched at PENDING_EXIT entry; fixed for that exit attempt (Relative END v1). */
typedef enum {
    DBMAP_ACTIVITY_EXIT_LATCH_NONE = 0,
    DBMAP_ACTIVITY_EXIT_LATCH_ABSOLUTE = 1,
    DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE = 2,
} dbmap_activity_exit_latch_t;

#define DBMAP_ACTIVITY_BASELINE_CAP 5u
#define DBMAP_ACTIVITY_BASELINE_MIN_SAMPLES 3u
#define DBMAP_ACTIVITY_ARMING_WINDOWS 2u

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

    /**
     * Relative END v1 (optional). When false, all relative fields are ignored and
     * absolute enter/exit/hold behavior is unchanged.
     */
    bool relative_end_enabled;
    /** Relative entry: rms <= frozen_baseline + return_margin_db. Provisional. */
    float return_margin_db;
    /** Relative cancel: rms > baseline + return_margin + cancel_hysteresis. Provisional. */
    float cancel_hysteresis_db;
    /** Hold while relative mode latched. Provisional field-test value. */
    uint32_t relative_exit_hold_ms;
    /** Min (event_peak - baseline) to arm relative END. Provisional. */
    float min_event_contrast_db;
    /** Drop baseline candidates older than this before freeze; 0 disables. */
    uint32_t baseline_stale_ms;
} dbmap_activity_config_t;

typedef struct {
    bool has_measurement;
    float rms_dbfs;
    /**
     * Peak dBFS for diagnostics only. Not used by enter/exit/baseline/arming.
     * Callers may leave at 0 when peak is unavailable (host tests).
     */
    float peak_dbfs;
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
    dbmap_activity_termination_t termination;
    float signal_level_dbfs;
    double duration_ms;
    int64_t interval_start_mono_us;
    int64_t interval_end_mono_us;
} dbmap_activity_emission_t;

typedef struct {
    float rms_dbfs;
    int64_t t_end_mono_us;
    uint32_t window_seq;
    bool valid;
} dbmap_activity_baseline_sample_t;

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

    /**
     * Natural END hold accumulator (absolute and relative).
     * Credits only continuous qualifying usable windows; invalid/missing gaps pause.
     * pending_since_us remains diagnostic and does not control natural END.
     */
    int64_t exit_hold_elapsed_us;
    int64_t exit_hold_anchor_us;
    uint32_t exit_hold_anchor_seq;
    bool exit_hold_anchor_valid;

    /* Relative END v1 state (unused when cfg.relative_end_enabled is false). */
    dbmap_activity_baseline_sample_t baseline_entries[DBMAP_ACTIVITY_BASELINE_CAP];
    uint32_t baseline_head;
    uint32_t baseline_count;
    bool baseline_frozen;
    bool baseline_reliable;
    float frozen_baseline_dbfs;
    uint32_t frozen_baseline_n;
    bool relative_armed;
    float arming_rms[DBMAP_ACTIVITY_ARMING_WINDOWS];
    uint32_t arming_count;
    float measured_contrast_db;
    dbmap_activity_exit_latch_t exit_latch;

    /** Diagnostic counters (do not affect detector decisions). */
    uint32_t pending_exit_enters;
    uint32_t pending_exit_cancels;
    /** Peak continuous exit_hold_elapsed_us observed in any PENDING_EXIT attempt. */
    int64_t exit_hold_elapsed_us_max;
    /**
     * SERIAL_DIAG only: count of post-boot-settle usable windows already logged
     * (capped; does not affect detection).
     */
    uint32_t post_settle_usable_diag_n;
} dbmap_activity_detector_t;

/**
 * Read-only diagnostic view of Absolute + Relative END runtime state.
 * Threshold fields reuse the same baseline/margin/hysteresis values as the SM.
 */
typedef struct {
    dbmap_activity_state_t state;
    bool relative_end_enabled;
    bool relative_armed;
    bool baseline_frozen;
    bool baseline_reliable;
    float frozen_baseline_dbfs;
    uint32_t frozen_baseline_n;
    float measured_contrast_db;
    /** Absolute enter/exit from cfg (unchanged). */
    float abs_enter_dbfs;
    float abs_exit_dbfs;
    /**
     * Relative thresholds from frozen baseline + cfg margins.
     * Valid only when baseline_frozen && baseline_reliable.
     */
    bool rel_thresholds_valid;
    float rel_exit_dbfs;
    float rel_cancel_dbfs;
    dbmap_activity_exit_latch_t exit_latch;
    int64_t exit_hold_elapsed_us;
    int64_t exit_hold_elapsed_us_max;
    uint32_t pending_exit_enters;
    uint32_t pending_exit_cancels;
    uint32_t latched_hold_ms;
} dbmap_activity_diag_t;

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
 * Ignores duplicate window_seq. Detects forward gaps and backward sequences
 * while PENDING_EXIT (invalidates the hold anchor; does not credit the gap).
 * Skips boot-settle and unusable samples without treating them as acoustic
 * quiet. May return at most one emission per call. MQTT must not gate this.
 */
dbmap_activity_emission_t dbmap_activity_detector_feed(
    dbmap_activity_detector_t *det,
    const dbmap_activity_sample_t *sample
);

dbmap_activity_state_t dbmap_activity_detector_state(const dbmap_activity_detector_t *det);

/**
 * Fill a diagnostic snapshot from detector runtime state (no side effects).
 * Relative threshold fields are derived from frozen_baseline_dbfs + cfg margins
 * exactly as PENDING_EXIT entry/stay use them.
 */
void dbmap_activity_detector_fill_diag(
    const dbmap_activity_detector_t *det,
    dbmap_activity_diag_t *out
);

/** Short name for exit latch (absolute / relative / none). */
const char *dbmap_activity_exit_latch_name(dbmap_activity_exit_latch_t latch);

/**
 * Format Relative END fields for serial diagnostics into ``buf``.
 * Returns bytes written (excluding NUL), or -1 on error.
 * Host-testable; used by ear_obs rate-limited snap.
 */
int dbmap_activity_format_rel_diag(const dbmap_activity_diag_t *diag, char *buf, size_t buflen);

/**
 * Force an END for an open episode (ACTIVE / PENDING_EXIT), ending at the last
 * usable sample when available. Returns NONE and leaves state unchanged for
 * IDLE / PENDING_ENTER. Does not count as invalid_hold.
 * Clears Relative END episode state.
 */
dbmap_activity_emission_t dbmap_activity_detector_force_end(dbmap_activity_detector_t *det);

/**
 * Reset detector to IDLE without emitting (silent abort / OTA no-END paths).
 * Clears Relative END episode state and baseline freeze.
 */
void dbmap_activity_detector_reset_idle(dbmap_activity_detector_t *det);

#ifdef __cplusplus
}
#endif
