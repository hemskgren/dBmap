#pragma once

/**
 * L2 Acoustic Dynamics (host-testable; contract version 1).
 *
 * Intra-window spectral_flux / envelope_cv plus inter-window dominant_delta_hz.
 * Not classification. See docs/diagnostics/ear-l2-acoustic-dynamics-contract.md.
 */

#include "dbmap_acoustic_l1.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBMAP_L2_MEASUREMENT_VERSION 1

/** Continuity gap tolerance around nominal analysis_window (contract D1). */
#define DBMAP_L2_CONTINUITY_TOL_US 100000LL

/** Frame RMS floor for envelope_cv frame membership (FS linear). */
#define DBMAP_L2_ENV_RMS_EPS 1.0e-6f

/**
 * Optional stage wall times (µs) from the last L2 intra path in
 * dbmap_l1_analyze_window_ex. rms_us covers raw-frame RMS only (not FFT/bins).
 */
typedef struct {
    uint32_t rms_us;
} dbmap_l2_analyze_stages_t;

void dbmap_l2_get_last_analyze_stages(dbmap_l2_analyze_stages_t *out);

/**
 * Intra-window metrics filled during the L1 FFT frame loop.
 * Optional out-arg for dbmap_l1_analyze_window_ex.
 */
struct dbmap_l2_intra_metrics {
    bool has_spectral_flux;
    float spectral_flux; /* mean d_i; dimensionless */
    uint32_t flux_pair_count; /* M */

    bool has_envelope_cv;
    float envelope_cv; /* population σ/μ of frame RMS */
    uint32_t envelope_rms_count; /* R */
};
typedef struct dbmap_l2_intra_metrics dbmap_l2_intra_metrics_t;

/** Per analysis window L2 (combined scene). */
typedef struct {
    bool eligible; /* L1 valid, unclipped, frame_count >= 2 */
    bool clipped;
    bool l1_valid;
    int64_t window_end_mono_us;
    uint32_t window_ms;
    uint32_t sample_count;
    uint32_t frame_count;

    float dominant_hz; /* from L1; meaningful when eligible */

    bool has_spectral_flux;
    float spectral_flux;

    bool has_envelope_cv;
    float envelope_cv;

    /** Set when continuity formed a pair ending at this window. */
    bool has_dominant_delta;
    float dominant_delta_hz;
} dbmap_l2_window_t;

/** One-window continuity cache for dominant_delta_hz (contract D2). */
typedef struct {
    bool has_prev;
    float prev_dominant_hz;
    int64_t prev_window_end_us;
} dbmap_l2_continuity_t;

void dbmap_l2_continuity_reset(dbmap_l2_continuity_t *cache);

/**
 * Update continuity from a just-analyzed window.
 * Forms Δf only when both windows are eligible and time-consecutive (±100 ms).
 * Clears cache on ineligible / gap; then stores current if eligible.
 * Sets window->has_dominant_delta / dominant_delta_hz.
 */
void dbmap_l2_continuity_push(
    dbmap_l2_continuity_t *cache,
    uint32_t analysis_window_ms,
    dbmap_l2_window_t *window
);

/** Interval-scoped L2 aggregate (matches acoustic.dynamics semantics). */
typedef struct {
    uint32_t sample_rate_hz;
    uint32_t analysis_window_ms;
    uint32_t fft_size;
    float freq_min_hz;
    float freq_max_hz;
    uint32_t frame_hop_samples;

    uint32_t window_count;
    uint32_t invalid_window_count;
    bool history_overflow;

    uint32_t spectral_flux_valid_count;
    bool has_spectral_flux_mean;
    float spectral_flux_mean;
    bool has_spectral_flux_stdev;
    float spectral_flux_stdev;

    uint32_t envelope_cv_valid_count;
    bool has_envelope_cv_mean;
    float envelope_cv_mean;
    bool has_envelope_cv_stdev;
    float envelope_cv_stdev;

    uint32_t dominant_delta_valid_count; /* pairs */
    bool has_dominant_delta_mean;
    float dominant_delta_mean_hz;
    bool has_dominant_delta_stdev;
    float dominant_delta_stdev_hz;

    int64_t interval_start_mono_us;
    int64_t interval_end_mono_us;
} dbmap_l2_interval_t;

typedef struct {
    uint32_t sample_rate_hz;
    uint32_t analysis_window_ms;
    int64_t interval_start_mono_us;
    int64_t interval_end_mono_us;

    uint32_t window_count;
    uint32_t invalid_window_count;
    bool history_overflow;

    uint32_t flux_n;
    double sum_flux;
    double sum_flux_sq;

    uint32_t env_n;
    double sum_env;
    double sum_env_sq;

    uint32_t delta_n;
    double sum_delta;
    double sum_delta_sq;
} dbmap_l2_interval_accum_t;

void dbmap_l2_interval_accum_init(
    dbmap_l2_interval_accum_t *acc,
    uint32_t sample_rate_hz,
    uint32_t analysis_window_ms,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us
);

/** Mark overflow: clears feature sums; finalize will emit Option A empty metrics. */
void dbmap_l2_interval_accum_mark_overflow(dbmap_l2_interval_accum_t *acc);

/**
 * Associate window if start < window_end <= end.
 * Eligible → window_count++; feature valid_counts independently.
 * Associated but !eligible → invalid_window_count++.
 * Dominant delta contributes only if has_dominant_delta (pair attributed here).
 */
void dbmap_l2_interval_accum_add(
    dbmap_l2_interval_accum_t *acc,
    const dbmap_l2_window_t *window
);

void dbmap_l2_interval_accum_finalize(
    const dbmap_l2_interval_accum_t *acc,
    dbmap_l2_interval_t *out
);

void dbmap_l2_interval_set_empty(
    dbmap_l2_interval_t *out,
    uint32_t sample_rate_hz,
    uint32_t analysis_window_ms,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us
);

/**
 * Analyze one PCM window: L1 features + L2 intra metrics in one FFT pass.
 * Fills l1_out (same semantics as dbmap_l1_analyze_window) and l2_out
 * (eligible / flux / envelope; dominant_delta left false until continuity_push).
 */
void dbmap_l2_analyze_window(
    dbmap_l1_workspace_t *ws,
    const int32_t *samples,
    size_t count,
    int full_scale_bits,
    uint32_t sample_rate_hz,
    uint32_t window_ms,
    int64_t window_end_mono_us,
    dbmap_l1_window_t *l1_out,
    dbmap_l2_window_t *l2_out
);

#ifdef __cplusplus
}
#endif
