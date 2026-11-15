#pragma once

/**
 * Sensor Platform L1 spectral features (host-testable).
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

#define SP_ACOUSTIC_L1_MEASUREMENT_VERSION 1
#define SP_ACOUSTIC_L1_FFT_SIZE 1024
#define SP_ACOUSTIC_L1_FFT_BINS ((SP_ACOUSTIC_L1_FFT_SIZE / 2) + 1)
#define SP_ACOUSTIC_L1_FREQ_MIN_HZ 80.0f
#define SP_ACOUSTIC_L1_FREQ_MAX_HZ 7600.0f

/** Relative total band power (vs full-scale^2) below which features are null. */
#define SP_ACOUSTIC_L1_VALID_ENERGY_FLOOR 1.0e-12

/** Peak within this many dB of 0 dBFS ⇒ clipped window (excluded from interval). */
#define SP_ACOUSTIC_L1_CLIP_PEAK_DBFS (-0.05f)

#ifndef SP_ACOUSTIC_L1_SILENCE_DBFS
#define SP_ACOUSTIC_L1_SILENCE_DBFS (-120.0f)
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
} sp_acoustic_l1_window_t;

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
} sp_acoustic_l1_interval_t;

/**
 * Static workspace for one analyze call (no heap).
 * Caller owns lifetime; may reuse across windows.
 * Includes precomputed Hann, bit-reversal, twiddle tables, and L2
 * intra-window spectral-flux scratch (sizeof ≈ 22540 bytes on typical ABI).
 * prev_power is owned by this workspace instance (not shared globally).
 */
typedef struct {
    float time_re[SP_ACOUSTIC_L1_FFT_SIZE];
    float time_im[SP_ACOUSTIC_L1_FFT_SIZE];
    float power_acc[SP_ACOUSTIC_L1_FFT_BINS];
    float hann[SP_ACOUSTIC_L1_FFT_SIZE];
    uint16_t bitrev[SP_ACOUSTIC_L1_FFT_SIZE];
    float tw_re[SP_ACOUSTIC_L1_FFT_SIZE / 2];
    float tw_im[SP_ACOUSTIC_L1_FFT_SIZE / 2];
    /** L2 flux only: previous-frame band power inside one analyze_window_ex call. */
    float prev_power[SP_ACOUSTIC_L1_FFT_BINS];
    bool tables_ready;
} sp_acoustic_l1_workspace_t;

void sp_acoustic_l1_workspace_init(sp_acoustic_l1_workspace_t *ws);

/** Optional L2 intra-window metrics (defined in sp_acoustic_l2.h). */
typedef struct sp_acoustic_l2_intra_metrics sp_acoustic_l2_intra_metrics_t;

/**
 * Analyze one PCM window (int32 samples at full_scale_bits, e.g. 24).
 * Uses non-overlapping Hann-windowed 1024-pt FFTs; averages power spectrum.
 * Sets valid=false on empty/insufficient energy; clipped when peak near FS.
 */
void sp_acoustic_l1_analyze_window(
    sp_acoustic_l1_workspace_t *ws,
    const int32_t *samples,
    size_t count,
    int full_scale_bits,
    uint32_t sample_rate_hz,
    uint32_t window_ms,
    int64_t window_end_mono_us,
    sp_acoustic_l1_window_t *out
);

/**
 * Same L1 analysis as sp_acoustic_l1_analyze_window; when intra_out != NULL also
 * computes spectral_flux / envelope_cv inside the FFT frame loop (no second FFT).
 * L1 outputs are unchanged vs sp_acoustic_l1_analyze_window for the same PCM.
 * When intra_out == NULL, delegates to sp_acoustic_l1_analyze_window (no L2 path).
 * Flux scratch uses ws->prev_power (workspace-owned, not task-stack locals).
 */
void sp_acoustic_l1_analyze_window_ex(
    sp_acoustic_l1_workspace_t *ws,
    const int32_t *samples,
    size_t count,
    int full_scale_bits,
    uint32_t sample_rate_hz,
    uint32_t window_ms,
    int64_t window_end_mono_us,
    sp_acoustic_l1_window_t *out,
    sp_acoustic_l2_intra_metrics_t *intra_out
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
} sp_acoustic_l1_interval_accum_t;

void sp_acoustic_l1_interval_accum_init(
    sp_acoustic_l1_interval_accum_t *acc,
    uint32_t sample_rate_hz,
    uint32_t analysis_window_ms,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us
);

/** Associate if start < window_end <= end. Counts invalid/clipped separately. */
void sp_acoustic_l1_interval_accum_add(
    sp_acoustic_l1_interval_accum_t *acc,
    const sp_acoustic_l1_window_t *window
);

void sp_acoustic_l1_interval_accum_finalize(
    const sp_acoustic_l1_interval_accum_t *acc,
    sp_acoustic_l1_interval_t *out
);

/** Empty but schema-valid interval object (window_count=0, features null). */
void sp_acoustic_l1_interval_set_empty(
    sp_acoustic_l1_interval_t *out,
    uint32_t sample_rate_hz,
    uint32_t analysis_window_ms,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us
);

#ifdef __cplusplus
}
#endif
