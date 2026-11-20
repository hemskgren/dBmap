/**
 * AS-001 — provisional helicopter-like profile (BBC NHU0509724).
 *
 * Template centers from EAR device-bench captures of
 * bbc_helicopter_nhu0509724.mp3 at Sonos volumes 0.25 / 0.45 / 0.65
 * (elevated windows vs pre-event background). Level eligibility uses
 * relative background margin (not absolute digital peak).
 * Mono stimulus / mono device path → pair disabled.
 * Not validated aircraft recognition / model ID.
 */
#include "as_profile.h"

/*
 * Device-bench elevated-window means (6 captures, rms >= bg+4 dB,
 * flatness <= 0.35):
 *   band_rel ≈ [-4.21, -3.52, -13.10, -32.77]
 *   dominant ≈ 230 Hz, flatness ≈ 0.141, centroid ≈ 800 Hz
 *
 * Absolute RMS is intentionally not part of the distance (multi-volume
 * playback). Relative-bg margin rejects ordinary office noise.
 */
static const as_profile_t s_as001 = {
    .id = AS_PROFILE_ID_AS001,
    .name = "AS-001",
    .acoustic_class = AS_CLASS_HELICOPTER_LIKE,
    .ae_schema_version = AE_SIGNATURE_SCHEMA_VERSION,
    .provisional = true,

    .uses_rms = false, /* amplitude-invariant distance; level via rel-bg */
    .uses_bands = true,
    .uses_spectral = true,
    .uses_pair = false, /* mono WAV / mono EAR; AE_FLAG_PAIR_MISSING */
    .uses_rms_rel_bg = true,

    .rms_center_dbfs = -38.0f, /* diagnostic only; not used in distance */
    .band_rel_center =
        {
            -4.2094f,
            -3.5240f,
            -13.0958f,
            -32.7691f,
        },
    .dominant_hz_center = 230.0f,
    .flatness_center = 0.1410f,
    .centroid_hz_center = 800.0f,
    .delta_rms_db_center = 0.0f,

    .rms_min_dbfs = -60.0f, /* unused when uses_rms_rel_bg */
    .rms_bg_margin_db = 3.5f,
    .dominant_hz_min = 100.0f,
    .dominant_hz_max = 450.0f,
    .flatness_min = 0.05f,
    .flatness_max = 0.32f,

    .match_distance_max = 6.0f,
};

const as_profile_t *as_profile_as001(void)
{
    return &s_as001;
}
