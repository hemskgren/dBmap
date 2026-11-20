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
    /**
     * When true, level eligibility is rms >= bg_ema + rms_bg_margin_db
     * (AS-002-style relative baseline). Absolute rms_min_dbfs is not used
     * for eligibility in this mode. requires as_profile_match_with_bg /
     * engine-owned bg state.
     */
    bool uses_rms_rel_bg;

    float rms_center_dbfs;
    float band_rel_center[AE_BAND_COUNT];
    float dominant_hz_center;
    float flatness_center;
    float centroid_hz_center;
    float delta_rms_db_center;

    /** Soft eligibility: RMS must be >= this when uses_rms && !uses_rms_rel_bg. */
    float rms_min_dbfs;
    /** Relative-bg eligibility margin (dB) when uses_rms_rel_bg. */
    float rms_bg_margin_db;
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

/**
 * Background EMA for relative-level eligibility (uses_rms_rel_bg profiles).
 * Updated on non-matching usable windows — same principle as AS-002's
 * non-tonal background, without sharing AS-002 siren gates.
 */
typedef struct {
    bool initialized;
    bool bg_valid;
    float bg_rms_ema;
} as_profile_bg_state_t;

void as_profile_bg_init(as_profile_bg_state_t *st);
void as_profile_bg_reset(as_profile_bg_state_t *st);

/** Built-in AS-001 (BBC heli / device-bench centered, mono / no pair). */
const as_profile_t *as_profile_as001(void);

/** Registry: null-terminated table of compiled-in profiles. */
size_t as_profile_count(void);
const as_profile_t *as_profile_get_by_index(size_t index);
const as_profile_t *as_profile_get_by_id(uint16_t id);

/**
 * Score sig against one profile (stateless).
 * Profiles with uses_rms_rel_bg reject eligibility here — use
 * as_profile_match_with_bg so a baseline can be established.
 */
bool as_profile_match(
    const as_profile_t *profile,
    const ae_signature_t *sig,
    as_profile_match_result_t *out
);

/**
 * Score with relative-bg state. When profile->uses_rms_rel_bg:
 *   - eligibility requires bg_valid and rms >= bg + margin
 *   - non-match windows update the EMA (alpha 0.85)
 * bg may be NULL only for profiles that do not use relative bg.
 */
bool as_profile_match_with_bg(
    const as_profile_t *profile,
    const ae_signature_t *sig,
    as_profile_bg_state_t *bg,
    as_profile_match_result_t *out
);

/** Best match among registered profiles (lowest distance that matches). */
bool as_profile_match_best(
    const ae_signature_t *sig,
    as_profile_match_result_t *out
);

/**
 * Best match with a shared bg state applied to every uses_rms_rel_bg
 * profile (call once per AE window). Non-rel profiles ignore bg.
 */
bool as_profile_match_best_with_bg(
    const ae_signature_t *sig,
    as_profile_bg_state_t *bg,
    as_profile_match_result_t *out
);

#ifdef __cplusplus
}
#endif
