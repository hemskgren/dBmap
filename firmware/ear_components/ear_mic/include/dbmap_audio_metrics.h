#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Host-testable audio window math. Levels are digital full-scale (dBFS), not dB SPL.
 * full_scale_bits is the signed peak magnitude bit width (e.g. 24 for INMP441 useful bits).
 */

float dbmap_audio_rms_dbfs(const int32_t *samples, size_t count, int full_scale_bits);
float dbmap_audio_peak_dbfs(const int32_t *samples, size_t count, int full_scale_bits);
uint32_t dbmap_audio_count_clipped(const int32_t *samples, size_t count, int full_scale_bits);

/**
 * I2S / capture-window health only (not microphone presence).
 *
 * expected_samples: rate_hz * window_ms / 1000 (0 → skip fill-ratio checks).
 * Returns "ok", "degraded", or "error" (never NULL).
 *
 * non_data_reads: hint for status only (e.g. timeout_reads + empty_reads). Telemetry
 * must still publish timeout / empty / hard-error counts as separate fields.
 *
 * Full digital silence with a healthy fill is still transport "ok" — use
 * dbmap_audio_classify_microphone for presence.
 */
const char *dbmap_audio_window_status(
    uint32_t sample_count,
    uint32_t expected_samples,
    uint32_t read_errors,
    uint32_t non_data_reads,
    uint32_t missing_data
);

/**
 * Optional window diagnostics for mic-presence investigation (no PCM payload).
 * unique_sample_count is capped; unique_capped marks overflow.
 */
typedef struct {
    int32_t raw_min;
    int32_t raw_max;
    uint32_t zero_sample_count;
    uint32_t full_scale_sample_count;
    uint32_t unique_sample_count;
    uint32_t consecutive_identical_samples; /* longest run of equal extracted samples */
    uint8_t unique_capped;
} dbmap_audio_window_diag_t;

/** Inputs for structure-based microphone presence classification. */
typedef struct {
    const char *transport_status;
    uint32_t sample_count;
    float rms_dbfs;
    float peak_dbfs;
    dbmap_audio_window_diag_t diag;
} dbmap_audio_mic_class_input_t;

/**
 * Structure-based microphone presence (not a single dBFS threshold).
 *
 * Returns (never NULL):
 * - "unavailable" — transport unusable / no samples
 * - "disconnected_suspected" — digital silence / near-all-zero
 * - "noise_suspected" — floating/rail/stuck pattern (e.g. EAR-001 no mic)
 * - "signal_present" — ambient-like energy distribution
 * - "unknown" — transport ok but ambiguous
 *
 * Writes a short machine-readable reason into reason_out when non-NULL.
 */
const char *dbmap_audio_classify_microphone(
    const dbmap_audio_mic_class_input_t *in,
    char *reason_out,
    size_t reason_out_size
);

/**
 * Legacy peak-only helper kept for older call sites/tests.
 * Prefer dbmap_audio_classify_microphone with full diagnostics.
 */
const char *dbmap_audio_microphone_status(
    const char *transport_status,
    float peak_dbfs
);

/** Expected PCM frames for a window (rounded). */
uint32_t dbmap_audio_expected_samples(uint32_t sample_rate_hz, uint32_t window_ms);

/**
 * INMP441: 24-bit PCM left-justified in a 32-bit I2S Philips slot (`>> 8`).
 * Matches INMP441 I2S 24-bit data in a wider frame; physical alignment on this
 * board remains unverified by scope — do not change without datasheet+measurement.
 */
int32_t dbmap_inmp441_extract_sample(uint32_t slot_word);

#ifdef __cplusplus
}
#endif
