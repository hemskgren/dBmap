#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Iteration 1: rolling background RMS history for Reported State.
 * Host-testable; publish/snapshot path must not mutate the ring.
 */

/** Same peak-near-FS rule as L0 clip classification (reuse, no new subsystem). */
#define DBMAP_BG_RMS_CLIP_PEAK_DBFS (-0.05f)

typedef struct {
    float rms_dbfs;
    int64_t t_end_mono_us;
} dbmap_bg_rms_sample_t;

typedef struct {
    dbmap_bg_rms_sample_t *entries;
    uint32_t capacity;
    uint32_t head;  /* next write index */
    uint32_t count;
    uint32_t window_ms;
    int64_t collection_start_mono_us; /* 0 = not started */
} dbmap_bg_rms_hist_t;

typedef struct {
    bool levels_valid;
    float rms_dbfs;
    float peak_dbfs;
    uint32_t clipped_samples;
    uint32_t window_seq; /* 1-based */
    uint32_t ms_since_i2s_enable;
    const char *transport_status;
    const char *microphone_status;
} dbmap_bg_rms_candidate_t;

typedef struct {
    bool present; /* false → omit entire background object */
    uint32_t window_ms;
    uint32_t sample_count;
    const char *status; /* "partial" | "full" when present */
    float rms_min_dbfs;
    float rms_max_dbfs;
    float rms_mean_dbfs; /* linear-mean energy → dBFS */
} dbmap_bg_rms_profile_t;

void dbmap_bg_rms_hist_init(
    dbmap_bg_rms_hist_t *hist,
    dbmap_bg_rms_sample_t *entries,
    uint32_t capacity,
    uint32_t window_ms
);

/**
 * Inclusion using existing transport/mic validity, clip signals, and startup
 * fields (window_seq / ms_since_i2s_enable). No new validity subsystem.
 */
bool dbmap_bg_rms_should_include(
    const dbmap_bg_rms_candidate_t *c,
    uint32_t boot_settle_ms
);

/**
 * Insert one included sample. May drop aged entries as an optimization;
 * correctness of Reported State does not depend on that (snapshot filters).
 */
void dbmap_bg_rms_push(
    dbmap_bg_rms_hist_t *hist,
    float rms_dbfs,
    int64_t t_end_mono_us
);

/**
 * Read-only profile: only samples with
 * t_end >= now_mono_us - window_ms. Does not mutate hist.
 * present=false when no in-window samples.
 */
void dbmap_bg_rms_profile(
    const dbmap_bg_rms_hist_t *hist,
    int64_t now_mono_us,
    dbmap_bg_rms_profile_t *out
);

/** Linear mean of RMS energy → dBFS (empty → false). */
bool dbmap_bg_rms_linear_mean_dbfs(
    const float *rms_dbfs,
    uint32_t count,
    float *out_mean_dbfs
);

#ifdef __cplusplus
}
#endif
