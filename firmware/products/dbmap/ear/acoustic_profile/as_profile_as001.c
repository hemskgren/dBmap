/**
 * AS-001 — provisional helicopter-like profile (heli_peak-centered).
 *
 * Values from docs/acoustic-profiles/AS-001/profile.json phases.heli_peak
 * (AE schema v1, host-extracted). Mono stimulus → pair disabled.
 * Not validated aircraft recognition.
 */
#include "as_profile.h"

/*
 * heli_peak means (profile.json):
 *   rms -9.9995
 *   band0 -18.2139 → rel -8.2144
 *   band1 -17.4205 → rel -7.4210
 *   band2 -26.9483 → rel -16.9488
 *   band3 -48.8432 → rel -38.8437
 *   dominant 492.1875, flatness 0.2566, centroid 1068.7055
 *
 * Eligibility gates widen slightly beyond peak to accept strong approach
 * windows while excluding background (~-34 dBFS, dominant ~100–200 Hz,
 * flatness ~0.60).
 */
static const as_profile_t s_as001 = {
    .id = AS_PROFILE_ID_AS001,
    .name = "AS-001",
    .acoustic_class = AS_CLASS_HELICOPTER_LIKE,
    .ae_schema_version = AE_SIGNATURE_SCHEMA_VERSION,
    .provisional = true,

    .uses_rms = true,
    .uses_bands = true,
    .uses_spectral = true,
    .uses_pair = false, /* mono WAV derivation; AE_FLAG_PAIR_MISSING */

    .rms_center_dbfs = -9.9995f,
    .band_rel_center =
        {
            -8.2144f,
            -7.4210f,
            -16.9488f,
            -38.8437f,
        },
    .dominant_hz_center = 492.1875f,
    .flatness_center = 0.2566f,
    .centroid_hz_center = 1068.7055f,
    .delta_rms_db_center = 0.0f,

    .rms_min_dbfs = -22.0f,
    .dominant_hz_min = 420.0f,
    .dominant_hz_max = 620.0f,
    .flatness_min = 0.15f,
    .flatness_max = 0.45f,

    .match_distance_max = 6.0f,
};

const as_profile_t *as_profile_as001(void)
{
    return &s_as001;
}
