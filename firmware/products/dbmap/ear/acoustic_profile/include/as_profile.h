/**
 * Acoustic profile registry + match (provisional).
 *
 * Profiles are class templates derived from AE signatures. They annotate
 * Candidates after Stage 1 identity association — they do not replace CE
 * identity tracking and do not promote Observations.
 */
#pragma once

#include "ae_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AS_PROFILE_ID_NONE 0u
#define AS_PROFILE_ID_AS001 1u
/** SE public-alert siren promote stamp (not an AS-001 profile match). */
#define AS_PROFILE_ID_AS002 2u

typedef enum {
    AS_CLASS_UNSPECIFIED = 0,
    AS_CLASS_HELICOPTER_LIKE = 1,
} as_acoustic_class_t;

/**
 * Firmware-usable profile template.
 * Centers are in the same units as ae_signature_t fields.
 * Relative band centers are (band_dbfs[i] - rms_dbfs).
 * Feature flags control which terms enter the distance; unavailable
 * features (e.g. pair on mono AS-001) must stay disabled.
 */
typedef struct {
    uint16_t id;
    const char *name;
    as_acoustic_class_t acoustic_class;
    uint32_t ae_schema_version;
    bool provisional;

    bool uses_rms;
    bool uses_bands;
    bool uses_spectral;
    bool uses_pair;

    float rms_center_dbfs;
    float band_rel_center[AE_BAND_COUNT];
    float dominant_hz_center;
    float flatness_center;
    float centroid_hz_center;
    float delta_rms_db_center;

    /** Soft eligibility: RMS must be >= this when uses_rms (rejects quiet bg). */
    float rms_min_dbfs;
    /**
     * Soft spectral eligibility when uses_spectral and sig.spectral_valid.
     * If max < min the gate is disabled for that metric.
     */
    float dominant_hz_min;
    float dominant_hz_max;
    float flatness_min;
    float flatness_max;

    float match_distance_max;
} as_profile_t;

typedef struct {
    bool matched;
    uint16_t profile_id;
    as_acoustic_class_t acoustic_class;
    float distance;
    /** Bits: why terms were skipped (diag / tests). */
    uint8_t skipped_spectral;
    uint8_t skipped_pair;
    uint8_t rejected_eligibility;
} as_profile_match_result_t;

/** Built-in AS-001 (heli_peak-centered, mono / no pair). */
const as_profile_t *as_profile_as001(void);

/** Registry: null-terminated table of compiled-in profiles. */
size_t as_profile_count(void);
const as_profile_t *as_profile_get_by_index(size_t index);
const as_profile_t *as_profile_get_by_id(uint16_t id);

/**
 * Score sig against one profile. Respects feature validity and profile
 * uses_* flags. Never invents pair/spectral zeros when unavailable.
 */
bool as_profile_match(
    const as_profile_t *profile,
    const ae_signature_t *sig,
    as_profile_match_result_t *out
);

/** Best match among registered profiles (lowest distance that matches). */
bool as_profile_match_best(
    const ae_signature_t *sig,
    as_profile_match_result_t *out
);

#ifdef __cplusplus
}
#endif
