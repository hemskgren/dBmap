#pragma once

/**
 * Step 2 Phase A — host-testable L0 acoustic measurements (combined mic scene).
 *
 * Describes the combined signal in an analysis window / Observation interval.
 * Not source separation, classification, or Acoustic Signature (L1).
 *
 * Timing: associate windows and compute T1/T2/T6 using monotonic microseconds only.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBMAP_L0_BAND_COUNT 4

/** Band edge / formula package for fractions and H/L. */
#define DBMAP_L0_BAND_PLAN_VERSION 1

/** Crest threshold package (provisional digital gate, not outdoor detection). */
#define DBMAP_L0_CREST_THRESHOLD_VERSION 1
#define DBMAP_L0_CREST_THRESHOLD_DB 12.0f

/** Overall L0 measurement semantics version. */
#define DBMAP_L0_MEASUREMENT_VERSION 1

/** Silence / undefined level floor (matches audio_metrics convention). */
#ifndef DBMAP_L0_SILENCE_DBFS
#define DBMAP_L0_SILENCE_DBFS (-120.0f)
#endif

/**
 * Relative total linear power floor (vs full-scale^2) below which band
 * fractions and H/L are undefined (returned as null via has_* flags).
 */
#define DBMAP_L0_VALID_ENERGY_FLOOR 1.0e-12

/** Peak within this many dB of 0 dBFS ⇒ treat window as clipped for T3. */
#define DBMAP_L0_CLIP_PEAK_DBFS (-0.05f)

typedef enum {
    DBMAP_L0_BAND_B0 = 0, /* 20–250 Hz */
    DBMAP_L0_BAND_B1 = 1, /* 250–1000 Hz */
    DBMAP_L0_BAND_B2 = 2, /* 1000–4000 Hz */
    DBMAP_L0_BAND_B3 = 3, /* 4000–8000 Hz */
} dbmap_l0_band_id_t;

typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float z1, z2;
} dbmap_l0_biquad_t;

/** One band: high-pass then low-pass (2nd-order Butterworth each). */
typedef struct {
    dbmap_l0_biquad_t hp;
    dbmap_l0_biquad_t lp;
} dbmap_l0_band_filter_t;

typedef struct {
    uint32_t sample_rate_hz;
    dbmap_l0_band_filter_t bands[DBMAP_L0_BAND_COUNT];
} dbmap_l0_filter_bank_t;

/** Per analysis window L0 (combined scene). */
typedef struct {
    bool valid;
    bool clipped;
    int64_t window_end_mono_us;
    uint32_t window_ms;
    uint32_t sample_count;

    float rms_dbfs;
    float peak_dbfs;
    float crest_db; /* peak_dbfs - rms_dbfs; undefined if !valid */

    /** Mean-square band power relative to full-scale^2 (linear). */
    double band_energy_linear[DBMAP_L0_BAND_COUNT];
    /** 10*log10(band_energy_linear), floored at SILENCE. */
    float band_dbfs[DBMAP_L0_BAND_COUNT];

    bool hl_valid;
    float hl_db; /* 10*log10((B2+B3)/(B0+B1)) */

    uint32_t clipped_sample_count;
} dbmap_l0_window_t;

/** Interval aggregates over usable associated windows. */
typedef struct {
    uint32_t window_count;          /* usable windows associated */
    uint32_t clipped_window_count;  /* usable but clipped (excluded from T3) */
    uint32_t analysis_window_ms;
    uint32_t sample_rate_hz;
    uint32_t mic_index;

    bool has_rms_stats;
    float rms_mean_dbfs;
    float rms_max_dbfs;
    float rms_min_dbfs;

    bool has_band_means;
    float band_mean_dbfs[DBMAP_L0_BAND_COUNT]; /* 10*log10(mean linear E_k) */

    bool has_band_fractions;
    float band_fraction[DBMAP_L0_BAND_COUNT]; /* from sum linear energies */

    /**
     * Interval H/L from aggregated linear energies (not mean of per-window dB):
     * 10*log10((ΣB2+ΣB3)/(ΣB0+ΣB1)). Null when energy below validity floor.
     */
    bool has_hl_mean;
    float hl_mean_db;

    /* T1 */
    bool has_t1;
    float rms_stdev_db; /* sample stdev of RMS dBFS */
    float rms_range_db; /* max - min */

    /* T2 */
    bool has_t2;
    float rms_slope_db_per_s;

    /* T3 */
    bool has_t3;
    float crest_max_db;
    uint32_t high_crest_count;
    float high_crest_rate; /* count / N_crest; only if has_t3 */
    uint32_t n_crest;      /* usable unclipped windows */

    /* T6 */
    bool has_t6;
    float hl_slope_db_per_s;

    int64_t interval_start_mono_us;
    int64_t interval_end_mono_us;

    /**
     * True if history capacity was exceeded while associating windows.
     * Temporal features (T1/T2/T3/T6) are cleared when set — never silently incomplete.
     */
    bool history_overflow;
} dbmap_l0_interval_t;

typedef struct {
    uint32_t sample_rate_hz;
    uint32_t analysis_window_ms;
    uint32_t mic_index;
    int64_t interval_start_mono_us;
    int64_t interval_end_mono_us;

    uint32_t window_count;
    uint32_t clipped_window_count;
    uint32_t n_crest;

    double sum_rms_dbfs;
    float rms_max_dbfs;
    float rms_min_dbfs;

    double sum_band_energy[DBMAP_L0_BAND_COUNT];

    /* For sample stdev / OLS — bounded host/firmware-friendly history. */
    uint32_t hist_count;
    uint32_t hist_cap;
    float *hist_rms_dbfs;
    float *hist_hl_db;
    uint8_t *hist_hl_valid;
    int64_t *hist_t_end_us;
    float *hist_crest_db;
    uint8_t *hist_crest_ok; /* usable and unclipped */

    /** Set when a usable associated window could not be stored in hist_* (cap). */
    bool history_overflow;
} dbmap_l0_interval_accum_t;

float dbmap_l0_band_low_hz(dbmap_l0_band_id_t band);
float dbmap_l0_band_high_hz(dbmap_l0_band_id_t band);

void dbmap_l0_filter_bank_init(dbmap_l0_filter_bank_t *bank, uint32_t sample_rate_hz);
void dbmap_l0_filter_bank_reset(dbmap_l0_filter_bank_t *bank);

/**
 * Last analyze_window stage wall times (µs). Updated each successful call.
 * Sum of stages should approximately match ear_mic analyze_us_* on device.
 */
typedef struct {
    uint32_t reset_us;
    uint32_t sample_loop_us;
    uint32_t post_us;
} dbmap_l0_analyze_stages_t;

void dbmap_l0_get_last_analyze_stages(dbmap_l0_analyze_stages_t *out);

/**
 * Analyze one PCM window (int32 samples at full_scale_bits, e.g. 24).
 * Resets filter state at the start of the window for deterministic window power.
 * Fills out; sets valid=false on empty input.
 */
void dbmap_l0_analyze_window(
    dbmap_l0_filter_bank_t *bank,
    const int32_t *samples,
    size_t count,
    int full_scale_bits,
    uint32_t window_ms,
    int64_t window_end_mono_us,
    dbmap_l0_window_t *out
);

/**
 * True when window_end belongs to the L0 aggregate for [start, end) Observation time:
 *   start < window_end_mono_us <= end
 * (exclusive start, inclusive end — includes the emission's closing window).
 */
bool dbmap_l0_window_in_interval(
    int64_t window_end_mono_us,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us
);

/**
 * Accumulator uses caller-owned history buffers (hist_cap entries each).
 * Pass NULL hist_* / hist_cap=0 only if you will not call add/finalize (invalid).
 */
void dbmap_l0_interval_accum_init(
    dbmap_l0_interval_accum_t *acc,
    uint32_t sample_rate_hz,
    uint32_t analysis_window_ms,
    uint32_t mic_index,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    float *hist_rms_dbfs,
    float *hist_hl_db,
    uint8_t *hist_hl_valid,
    int64_t *hist_t_end_us,
    float *hist_crest_db,
    uint8_t *hist_crest_ok,
    uint32_t hist_cap
);

/** Add window if valid and monotonic-associated; ignore otherwise. */
void dbmap_l0_interval_accum_add(
    dbmap_l0_interval_accum_t *acc,
    const dbmap_l0_window_t *window
);

void dbmap_l0_interval_accum_finalize(
    const dbmap_l0_interval_accum_t *acc,
    dbmap_l0_interval_t *out
);

#ifdef __cplusplus
}
#endif
