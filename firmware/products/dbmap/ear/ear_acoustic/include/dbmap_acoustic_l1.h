#pragma once

/**
 * Step 3 — L1 Acoustic Signature (host-testable).
 *
 * Compact spectral description of the combined mic scene for an analysis
 * window / Observation report interval. Not classification (L2).
 *
 * FFT: 1024-pt real, Hann, non-overlapping frames, analysis band 80–7600 Hz.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBMAP_L1_MEASUREMENT_VERSION 1
#define DBMAP_L1_FFT_SIZE 1024
#define DBMAP_L1_FFT_BINS ((DBMAP_L1_FFT_SIZE / 2) + 1)
#define DBMAP_L1_FREQ_MIN_HZ 80.0f
#define DBMAP_L1_FREQ_MAX_HZ 7600.0f

/** Relative total band power (vs full-scale^2) below which features are null. */
#define DBMAP_L1_VALID_ENERGY_FLOOR 1.0e-12

/** Peak within this many dB of 0 dBFS ⇒ clipped window (excluded from interval). */
#define DBMAP_L1_CLIP_PEAK_DBFS (-0.05f)

#ifndef DBMAP_L1_SILENCE_DBFS
#define DBMAP_L1_SILENCE_DBFS (-120.0f)
#endif

/** Per-window L1 features (combined scene). */
typedef struct {
    bool valid;
    bool clipped;
    int64_t window_end_mono_us;
    uint32_t window_ms;
    uint32_t sample_count;
    uint32_t frame_count;

    float centroid_hz;
    float bandwidth_hz;
    float dominant_hz;
    float flatness; /* geometric_mean(mag) / arithmetic_mean(mag), 0..1 */
} dbmap_l1_window_t;

/** Interval aggregates over valid unclipped associated windows. */
typedef struct {
    uint32_t window_count;         /* valid unclipped associated */
    uint32_t invalid_window_count; /* associated but !valid or clipped */
    uint32_t analysis_window_ms;
    uint32_t sample_rate_hz;
    uint32_t fft_size;
    float freq_min_hz;
    float freq_max_hz;

    bool has_centroid;
    float centroid_mean_hz;
    float centroid_stdev_hz; /* null unless window_count >= 2 */

    bool has_bandwidth;
    float bandwidth_mean_hz;
    float bandwidth_stdev_hz;

    bool has_dominant;
    float dominant_mean_hz;
    float dominant_stdev_hz;

    bool has_flatness;
    float flatness_mean;
    float flatness_stdev;

    bool has_stdev; /* true when window_count >= 2 */

    int64_t interval_start_mono_us;
    int64_t interval_end_mono_us;
} dbmap_l1_interval_t;

/**
 * Static workspace for one analyze call (no heap).
 * Caller owns lifetime; may reuse across windows.
 * Includes precomputed Hann, bit-reversal, twiddle tables, and L2
 * intra-window spectral-flux scratch (sizeof ≈ 22540 bytes on typical ABI).
 * prev_power is owned by this workspace instance (not shared globally).
 */
typedef struct {
    float time_re[DBMAP_L1_FFT_SIZE];
    float time_im[DBMAP_L1_FFT_SIZE];
    float power_acc[DBMAP_L1_FFT_BINS];
    float hann[DBMAP_L1_FFT_SIZE];
    uint16_t bitrev[DBMAP_L1_FFT_SIZE];
    float tw_re[DBMAP_L1_FFT_SIZE / 2];
    float tw_im[DBMAP_L1_FFT_SIZE / 2];
    /** L2 flux only: previous-frame band power inside one analyze_window_ex call. */
    float prev_power[DBMAP_L1_FFT_BINS];
    bool tables_ready;
} dbmap_l1_workspace_t;

void dbmap_l1_workspace_init(dbmap_l1_workspace_t *ws);

/** Optional L2 intra-window metrics (defined in dbmap_acoustic_l2.h). */
typedef struct dbmap_l2_intra_metrics dbmap_l2_intra_metrics_t;

/**
 * Analyze one PCM window (int32 samples at full_scale_bits, e.g. 24).
 * Uses non-overlapping Hann-windowed 1024-pt FFTs; averages power spectrum.
 * Sets valid=false on empty/insufficient energy; clipped when peak near FS.
 */
void dbmap_l1_analyze_window(
    dbmap_l1_workspace_t *ws,
    const int32_t *samples,
    size_t count,
    int full_scale_bits,
    uint32_t sample_rate_hz,
    uint32_t window_ms,
    int64_t window_end_mono_us,
    dbmap_l1_window_t *out
);

/**
 * Same L1 analysis as dbmap_l1_analyze_window; when intra_out != NULL also
 * computes spectral_flux / envelope_cv inside the FFT frame loop (no second FFT).
 * L1 outputs are unchanged vs dbmap_l1_analyze_window for the same PCM.
 * When intra_out == NULL, delegates to dbmap_l1_analyze_window (no L2 path).
 * Flux scratch uses ws->prev_power (workspace-owned, not task-stack locals).
 */
void dbmap_l1_analyze_window_ex(
    dbmap_l1_workspace_t *ws,
    const int32_t *samples,
    size_t count,
    int full_scale_bits,
    uint32_t sample_rate_hz,
    uint32_t window_ms,
    int64_t window_end_mono_us,
    dbmap_l1_window_t *out,
    dbmap_l2_intra_metrics_t *intra_out
);

typedef struct {
    uint32_t sample_rate_hz;
    uint32_t analysis_window_ms;
    int64_t interval_start_mono_us;
    int64_t interval_end_mono_us;

    uint32_t window_count;
    uint32_t invalid_window_count;

    double sum_centroid;
    double sum_bandwidth;
    double sum_dominant;
    double sum_flatness;
    double sum_centroid_sq;
    double sum_bandwidth_sq;
    double sum_dominant_sq;
    double sum_flatness_sq;
} dbmap_l1_interval_accum_t;

void dbmap_l1_interval_accum_init(
    dbmap_l1_interval_accum_t *acc,
    uint32_t sample_rate_hz,
    uint32_t analysis_window_ms,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us
);

/** Associate if start < window_end <= end. Counts invalid/clipped separately. */
void dbmap_l1_interval_accum_add(
    dbmap_l1_interval_accum_t *acc,
    const dbmap_l1_window_t *window
);

void dbmap_l1_interval_accum_finalize(
    const dbmap_l1_interval_accum_t *acc,
    dbmap_l1_interval_t *out
);

/** Empty but schema-valid interval object (window_count=0, features null). */
void dbmap_l1_interval_set_empty(
    dbmap_l1_interval_t *out,
    uint32_t sample_rate_hz,
    uint32_t analysis_window_ms,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us
);

#ifdef __cplusplus
}
#endif
