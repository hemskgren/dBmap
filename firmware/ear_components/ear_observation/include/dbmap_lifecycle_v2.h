#pragma once

/**
 * Experimental Observation lifecycle v2 (host-testable).
 *
 * Separates:
 * - Activity gate (RMS change detection)
 * - Signature score (explainable tonal vs impulsive scene descriptors)
 * - Observation lifecycle (START / PERIODIC / END)
 *
 * Uses existing L0/L1/L2 scalar features only — no FFT/autocorr/ML.
 * Score is NOT a probability or classification confidence.
 */

#include "dbmap_activity_detector.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBMAP_LC_V2_SCORE_HIST_CAP 8u

typedef struct {
    /** Activity gate: enter when rms >= gate_enter_dbfs. */
    float gate_enter_dbfs;
    /** Absolute quiet fallback END when rms < quiet_fallback_dbfs. */
    float quiet_fallback_dbfs;
    uint32_t min_activity_ms;
    uint32_t boot_settle_ms;
    uint32_t observation_interval_ms;
    uint32_t max_invalid_hold_ms;

    /**
     * Signature score enter/exit (instant → persistent mean).
     * Enter episode requires gate; END prefers persistent_score < score_exit.
     */
    float score_enter;
    float score_exit;
    /** Windows in the persistent mean (1..DBMAP_LC_V2_SCORE_HIST_CAP). */
    uint32_t persistence_windows;
    uint32_t signature_exit_hold_ms;
    uint32_t quiet_exit_hold_ms;
    /** Hard cap; END with NATURAL when exceeded (Hub has no max_age reason). */
    uint32_t max_episode_ms;
    /** Include this many ms before gate start in START interval association. */
    uint32_t pre_trigger_ms;
} dbmap_lifecycle_v2_config_t;

/**
 * One completed analysis window of scalar features (already computed).
 * Missing L1/L2 → features_valid=false (score path pauses).
 */
typedef struct {
    dbmap_activity_sample_t sample; /* RMS + usability fields */

    bool features_valid;
    float crest_db;
    float flatness;
    bool has_flatness;
    float spectral_flux;
    bool has_spectral_flux;
    float dominant_hz;
    bool has_dominant;
    float dominant_delta_hz;
    bool has_dominant_delta;
} dbmap_lifecycle_v2_window_t;

typedef enum {
    DBMAP_LC_V2_IDLE = 0,
    DBMAP_LC_V2_PENDING_ENTER,
    DBMAP_LC_V2_ACTIVE,
    DBMAP_LC_V2_PENDING_EXIT,
} dbmap_lifecycle_v2_state_t;

typedef enum {
    DBMAP_LC_V2_EXIT_NONE = 0,
    DBMAP_LC_V2_EXIT_SIGNATURE = 1,
    DBMAP_LC_V2_EXIT_QUIET = 2,
} dbmap_lifecycle_v2_exit_latch_t;

typedef struct {
    float tonality;       /* 1 - flatness */
    float dom_stability;  /* 1 - |Δf|/ref */
    float low_flux;       /* 1 - flux/ref */
    float low_crest;      /* 1 - crest excess */
    float instant;        /* weighted combination in [0,1] */
    float persistent;     /* mean of last persistence_windows instants */
    bool features_valid;
} dbmap_lifecycle_v2_score_t;

typedef struct {
    dbmap_lifecycle_v2_config_t cfg;
    dbmap_lifecycle_v2_state_t state;
    bool boot_complete;

    int64_t pending_since_us;
    int64_t episode_start_us; /* first gate window start (pre-trigger applied on START) */
    int64_t last_emit_us;
    int64_t last_valid_us;
    float last_level_dbfs;
    uint32_t last_window_seq;
    bool has_last_window;
    bool has_last_valid;

    float score_hist[DBMAP_LC_V2_SCORE_HIST_CAP];
    uint32_t score_hist_count;
    uint32_t score_hist_head;
    dbmap_lifecycle_v2_score_t last_score;

    dbmap_lifecycle_v2_exit_latch_t exit_latch;
    int64_t exit_hold_elapsed_us;
    int64_t exit_hold_anchor_us;
    uint32_t exit_hold_anchor_seq;
    bool exit_hold_anchor_valid;

    bool end_emitted; /* exactly-one-END guard within episode */

    uint32_t windows_seen;
    uint32_t windows_invalid;
    uint32_t windows_boot_settle;
    uint32_t emissions_start;
    uint32_t emissions_periodic;
    uint32_t emissions_end;
    uint32_t forced_ends_invalid;
    uint32_t ends_max_age;
    uint32_t ends_signature;
    uint32_t ends_quiet;
} dbmap_lifecycle_v2_t;

bool dbmap_lifecycle_v2_config_valid(const dbmap_lifecycle_v2_config_t *cfg);

/** Default experimental config (matches Kconfig defaults when enabled). */
void dbmap_lifecycle_v2_config_set_defaults(dbmap_lifecycle_v2_config_t *cfg);

void dbmap_lifecycle_v2_init(
    dbmap_lifecycle_v2_t *lc,
    const dbmap_lifecycle_v2_config_t *cfg
);

/**
 * Compute explainable score components from one window.
 * Invalid/missing features → features_valid=false, instant=0 (not used in mean).
 */
dbmap_lifecycle_v2_score_t dbmap_lifecycle_v2_score_window(
    const dbmap_lifecycle_v2_window_t *win
);

/**
 * Feed one completed window. Returns at most one emission (same shape as v1).
 * START interval_start includes pre_trigger_ms lookback.
 */
dbmap_activity_emission_t dbmap_lifecycle_v2_feed(
    dbmap_lifecycle_v2_t *lc,
    const dbmap_lifecycle_v2_window_t *win
);

dbmap_lifecycle_v2_state_t dbmap_lifecycle_v2_state(const dbmap_lifecycle_v2_t *lc);

dbmap_activity_emission_t dbmap_lifecycle_v2_force_end(dbmap_lifecycle_v2_t *lc);

void dbmap_lifecycle_v2_reset_idle(dbmap_lifecycle_v2_t *lc);

/** Map v2 state onto activity states for shared OTA close helper. */
dbmap_activity_state_t dbmap_lifecycle_v2_as_activity_state(
    dbmap_lifecycle_v2_state_t state
);

#ifdef __cplusplus
}
#endif
