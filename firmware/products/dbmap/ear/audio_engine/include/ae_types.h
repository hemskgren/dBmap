/**
 * Audio Engine v1 types (acoustic signatures).
 *
 * Temporary product placement under products/dbmap/ear/ (§0.9) — not a
 * permanent Platform contract. Domain types stay Engine-owned.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AE_SIGNATURE_SCHEMA_VERSION 1u
#define AE_BAND_COUNT 4u

typedef enum {
    AE_QUALITY_OK = 0,
    AE_QUALITY_DEGRADED = 1,
    AE_QUALITY_GAP = 2,
    AE_QUALITY_INVALID = 3,
} ae_quality_t;

#define AE_FLAG_CLIPPED (1u << 0)
#define AE_FLAG_STEREO_DEGRADED (1u << 1)
#define AE_FLAG_L1_MISSING (1u << 2)
#define AE_FLAG_L2_MISSING (1u << 3)
#define AE_FLAG_PAIR_MISSING (1u << 4)

/**
 * Versioned, quality-gated feature package for one analysis window.
 * Describes what was heard — not object identity or classification.
 */
typedef struct {
    uint32_t schema_version;
    uint32_t window_seq;
    int64_t window_end_mono_us;
    uint32_t window_ms;
    uint8_t mic_mask; /* bit0=mic0, bit1=mic1, … */
    uint8_t quality;  /* ae_quality_t */
    uint8_t flags;

    float rms_dbfs;
    float peak_dbfs;
    float crest_db;
    float band_dbfs[AE_BAND_COUNT];
    float hl_db;
    bool hl_valid;

    float centroid_hz;
    float bandwidth_hz;
    float dominant_hz;
    float flatness;
    bool spectral_valid;

    float spectral_flux;
    float envelope_cv;
    float dominant_delta_hz;
    bool dominant_delta_valid;
    bool dynamics_valid;

    float delta_rms_db;
    float delta_peak_db;
    bool pair_valid;
} ae_signature_t;

/** Optional mic0/mic1 pair metrics from Platform channel snapshots (not TDOA). */
typedef struct {
    bool valid;
    float mic0_rms_dbfs;
    float mic1_rms_dbfs;
    float mic0_peak_dbfs;
    float mic1_peak_dbfs;
    uint8_t mic_mask;
    bool stereo_degraded;
} ae_pair_metrics_t;

#ifdef __cplusplus
}
#endif
