/**
 * Host-only prototype activity detector with frozen pre-event baseline END.
 * Reuses production sample/emission types; does not modify firmware sources.
 */
#pragma once

#include "dbmap_activity_detector.h"
#include "proto_pre_event_baseline.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROTO_EXIT_MODE_ABSOLUTE = 0,           /* reference: current −55 path */
    PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK, /* relative if reliable else absolute */
} proto_exit_mode_t;

/** How event contrast is measured before relative END is armed. */
typedef enum {
    PROTO_CONTRAST_DISABLED = 0,
    /** Contrast from the START emission window only. */
    PROTO_CONTRAST_START_WINDOW = 1,
    /** Max RMS over the first arming_windows usable samples of the episode. */
    PROTO_CONTRAST_ARMING_MAX = 2,
    /** Median RMS over the first arming_windows usable samples of the episode. */
    PROTO_CONTRAST_ARMING_MEDIAN = 3,
    /**
     * Design alt1 (host study): arm from episode peak so far while ACTIVE.
     * Uses event_peak_dbfs updated every usable ACTIVE/PENDING_EXIT window.
     */
    PROTO_CONTRAST_EPISODE_PEAK = 4,
} proto_contrast_method_t;

/**
 * Ambient-increase policy while ACTIVE (Investigation B / Rel END v2 study).
 */
typedef enum {
    PROTO_AMBIENT_POLICY_NONE = 0,       /* accept delayed/missing relative END */
    PROTO_AMBIENT_POLICY_INDICATOR = 1,  /* set flag only; END rules unchanged */
    /**
     * After sustained below-enter, low-variance windows that sit well above the
     * frozen baseline, allow END against that secondary reference.
     * Does not rewrite frozen_baseline_dbfs.
     */
    PROTO_AMBIENT_POLICY_SECONDARY_EXIT = 2,
    /**
     * Design alt2 (host study): same stability/jump gate as secondary, but
     * rewrites frozen_baseline_dbfs to the stable median (refresh). Relative
     * entry/cancel then use the refreshed baseline.
     */
    PROTO_AMBIENT_POLICY_REFRESH_BASELINE = 3,
} proto_ambient_policy_t;

typedef struct {
    dbmap_activity_config_t abs_cfg; /* enter/min/periodic/invalid/boot; exit used as fallback */
    proto_exit_mode_t exit_mode;
    float return_margin_db;
    float cancel_hysteresis_db; /* leave PENDING_EXIT when rms > baseline+margin+this */
    uint32_t relative_exit_hold_ms; /* hold for relative path; abs_cfg.exit_hold_ms for absolute */
    uint32_t stale_max_ms;

    /* Investigation A — min event contrast (0 disables via CONTRAST_DISABLED) */
    proto_contrast_method_t contrast_method;
    float min_event_contrast_db;
    uint32_t contrast_arming_windows; /* for MAX/MEDIAN; typically 2–3 */

    /* Investigation B — ambient shift */
    proto_ambient_policy_t ambient_policy;
    float ambient_jump_db;           /* secondary/refresh needs level >= baseline + this */
    uint32_t ambient_stable_windows;  /* consecutive below-enter windows */
    float ambient_stable_range_db;    /* max-min within stable window set */

    /**
     * Alt1 safety: for EPISODE_PEAK, only arm when the peak sample is among the
     * last recent_peak_windows usable episode windows (0 = any-time peak).
     */
    uint32_t recent_peak_windows;

    /**
     * Alt3b (host study): while ACTIVE, if rms < enter for this many ms of
     * continuous below-enter dwell, emit natural END even when unarmed.
     * 0 disables. Intended for absolute-hysteresis stuck cases (E3/E5).
     */
    uint32_t below_enter_end_hold_ms;

    /**
     * Investigation C — optional reuse of prior freeze when new freeze unreliable.
     * 0 disables. If >0 and second START has unreliable baseline, copy prior
     * freeze if it ended within this many ms (host evaluation only).
     */
    uint32_t reuse_prior_baseline_max_age_ms;
} proto_relative_config_t;

typedef struct {
    proto_relative_config_t cfg;
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
    proto_pre_event_baseline_t baseline;

    /* Contrast arming */
    bool relative_armed;
    float event_peak_dbfs;
    float arming_rms[8];
    uint32_t arming_count;
    float measured_contrast_db;
    bool contrast_blocked_end; /* relative would have fired but unarmed */

    /* Ambient indicator / secondary (frozen baseline unchanged) */
    bool ambient_shift_suspected;
    float secondary_ambient_dbfs;
    bool secondary_ambient_valid;
    float amb_stable_rms[8];
    uint32_t amb_stable_count;
    bool last_end_used_secondary;
    bool last_end_used_refresh;
    bool last_end_used_below_enter;

    /* EPISODE_PEAK recent-window ring (host study). */
    float recent_rms[16];
    uint32_t recent_count;
    uint32_t recent_head;
    uint32_t episode_usable_n;

    /* below-enter dwell END (alt3b) */
    int64_t below_enter_anchor_us;
    bool below_enter_anchor_valid;
    int64_t below_enter_elapsed_us;

    /* Prior baseline reuse across END→START */
    bool have_prior_baseline;
    float prior_baseline_dbfs;
    uint32_t prior_baseline_n;
    int64_t prior_end_us;
    bool reused_prior_baseline;

    /* Exit path used for current / last END */
    bool last_end_used_fallback;
    bool last_end_used_relative;
    bool last_end_relative_armed; /* armed state at END, before clear */
    float last_end_contrast_db;
    uint32_t emissions_start;
    uint32_t emissions_periodic;
    uint32_t emissions_end;
    uint32_t forced_ends_invalid;
} proto_relative_detector_t;

void proto_relative_detector_init(
    proto_relative_detector_t *det,
    const proto_relative_config_t *cfg
);

dbmap_activity_emission_t proto_relative_detector_feed(
    proto_relative_detector_t *det,
    const dbmap_activity_sample_t *sample
);

dbmap_activity_state_t proto_relative_detector_state(const proto_relative_detector_t *det);

/** True when relative END predicate is allowed (reliable baseline and armed). */
bool proto_relative_detector_relative_armed(const proto_relative_detector_t *det);

#ifdef __cplusplus
}
#endif
