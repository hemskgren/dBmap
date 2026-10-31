#include "dbmap_audio_metrics.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef DBMAP_AUDIO_SILENCE_DBFS
#define DBMAP_AUDIO_SILENCE_DBFS (-120.0f)
#endif

static double full_scale_magnitude(int full_scale_bits)
{
    if (full_scale_bits < 2) {
        full_scale_bits = 2;
    }
    if (full_scale_bits > 31) {
        full_scale_bits = 31;
    }
    return (double)(1LL << (full_scale_bits - 1));
}

static float to_dbfs(double magnitude, double full_scale)
{
    if (!(magnitude > 0.0) || !(full_scale > 0.0)) {
        return DBMAP_AUDIO_SILENCE_DBFS;
    }
    double ratio = magnitude / full_scale;
    if (ratio <= 0.0) {
        return DBMAP_AUDIO_SILENCE_DBFS;
    }
    double dbfs = 20.0 * log10(ratio);
    if (dbfs < DBMAP_AUDIO_SILENCE_DBFS) {
        return DBMAP_AUDIO_SILENCE_DBFS;
    }
    if (dbfs > 0.0) {
        return 0.0f;
    }
    return (float)dbfs;
}

float dbmap_audio_rms_dbfs(const int32_t *samples, size_t count, int full_scale_bits)
{
    if (samples == NULL || count == 0) {
        return DBMAP_AUDIO_SILENCE_DBFS;
    }
    double sum_sq = 0.0;
    for (size_t i = 0; i < count; ++i) {
        double v = (double)samples[i];
        sum_sq += v * v;
    }
    double rms = sqrt(sum_sq / (double)count);
    return to_dbfs(rms, full_scale_magnitude(full_scale_bits));
}

float dbmap_audio_peak_dbfs(const int32_t *samples, size_t count, int full_scale_bits)
{
    if (samples == NULL || count == 0) {
        return DBMAP_AUDIO_SILENCE_DBFS;
    }
    int32_t peak = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t v = samples[i];
        if (v < 0) {
            /* Avoid undefined abs(INT32_MIN). */
            v = (v == INT32_MIN) ? INT32_MAX : -v;
        }
        if (v > peak) {
            peak = v;
        }
    }
    return to_dbfs((double)peak, full_scale_magnitude(full_scale_bits));
}

uint32_t dbmap_audio_count_clipped(const int32_t *samples, size_t count, int full_scale_bits)
{
    if (samples == NULL || count == 0) {
        return 0;
    }
    double fs = full_scale_magnitude(full_scale_bits);
    /* Treat near-full-scale as clipped; meaningful only after alignment is verified. */
    int32_t threshold = (int32_t)(fs * 0.99);
    if (threshold < 1) {
        threshold = 1;
    }
    uint32_t clipped = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t v = samples[i];
        if (v < 0) {
            v = (v == INT32_MIN) ? INT32_MAX : -v;
        }
        if (v >= threshold) {
            clipped++;
        }
    }
    return clipped;
}

uint32_t dbmap_audio_expected_samples(uint32_t sample_rate_hz, uint32_t window_ms)
{
    if (sample_rate_hz == 0 || window_ms == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)sample_rate_hz * (uint64_t)window_ms) / 1000ull);
}

const char *dbmap_audio_window_status(
    uint32_t sample_count,
    uint32_t expected_samples,
    uint32_t read_errors,
    uint32_t non_data_reads,
    uint32_t missing_data
)
{
    if (missing_data > 0 || sample_count == 0) {
        return "error";
    }
    /* Less than 10% of the window filled ⇒ starved I2S / not delivering. */
    if (expected_samples > 0 && sample_count < (expected_samples / 10u)) {
        return "error";
    }
    const bool thin =
        expected_samples > 0 && sample_count < (expected_samples / 2u);
    const bool noisy =
        read_errors > 0 || non_data_reads > 0;
    if (thin || noisy) {
        return "degraded";
    }
    return "ok";
}

static void set_reason(char *reason_out, size_t reason_out_size, const char *reason)
{
    if (reason_out == NULL || reason_out_size == 0) {
        return;
    }
    if (reason == NULL) {
        reason_out[0] = '\0';
        return;
    }
    strncpy(reason_out, reason, reason_out_size - 1);
    reason_out[reason_out_size - 1] = '\0';
}

const char *dbmap_audio_classify_microphone(
    const dbmap_audio_mic_class_input_t *in,
    char *reason_out,
    size_t reason_out_size
)
{
    if (in == NULL) {
        set_reason(reason_out, reason_out_size, "null_input");
        return "unavailable";
    }

    const bool transport_usable =
        in->transport_status != NULL
        && (strcmp(in->transport_status, "ok") == 0
            || strcmp(in->transport_status, "degraded") == 0);
    if (!transport_usable) {
        set_reason(reason_out, reason_out_size, "transport_unusable");
        return "unavailable";
    }
    if (in->sample_count == 0) {
        set_reason(reason_out, reason_out_size, "no_samples");
        return "unavailable";
    }

    const double n = (double)in->sample_count;
    const double zero_ratio = (double)in->diag.zero_sample_count / n;
    const double fs_ratio = (double)in->diag.full_scale_sample_count / n;
    const double identical_ratio = (double)in->diag.consecutive_identical_samples / n;
    const uint32_t unique = in->diag.unique_sample_count;
    const bool unique_rich = in->diag.unique_capped != 0 || unique >= 64u;
    const bool peak_silence = !(in->peak_dbfs > DBMAP_AUDIO_SILENCE_DBFS);

    /* Digital silence (EAR-002…005 with floating DIN held quiet). */
    if (peak_silence || zero_ratio >= 0.995) {
        set_reason(reason_out, reason_out_size, "digital_silence");
        return "disconnected_suspected";
    }

    /*
     * Floating / electrically noisy DIN (EAR-001 observed pattern):
     * mostly exact zeros, rare 24-bit rail hits, low unique count, long identical runs.
     * Nonzero peak/RMS alone must not become signal_present.
     */
    if (zero_ratio >= 0.85 && in->diag.full_scale_sample_count >= 1u) {
        set_reason(reason_out, reason_out_size, "mostly_zero_with_rail_hits");
        return "noise_suspected";
    }
    if (zero_ratio >= 0.85 && unique < 64u && in->diag.unique_capped == 0) {
        set_reason(reason_out, reason_out_size, "mostly_zero_low_unique");
        return "noise_suspected";
    }
    if (identical_ratio >= 0.25 && unique < 128u && in->diag.unique_capped == 0) {
        set_reason(reason_out, reason_out_size, "long_identical_runs");
        return "noise_suspected";
    }
    if (fs_ratio >= 0.05) {
        set_reason(reason_out, reason_out_size, "rail_heavy");
        return "noise_suspected";
    }
    if (in->peak_dbfs >= -0.05f && zero_ratio >= 0.50) {
        set_reason(reason_out, reason_out_size, "peak_rail_sparse_energy");
        return "noise_suspected";
    }

    /*
     * Ambient-like energy: enough value diversity, not dominated by exact zeros or rails.
     * Quiet live mics usually have many near-zero but not ~95% exact zeros with FS spikes.
     */
    if (unique_rich
        && zero_ratio < 0.85
        && fs_ratio < 0.02
        && identical_ratio < 0.25
        && in->peak_dbfs > DBMAP_AUDIO_SILENCE_DBFS) {
        set_reason(reason_out, reason_out_size, "ambient_like_energy");
        return "signal_present";
    }

    set_reason(reason_out, reason_out_size, "ambiguous_window");
    return "unknown";
}

const char *dbmap_audio_microphone_status(
    const char *transport_status,
    float peak_dbfs
)
{
    /* Legacy: peak-only. Prefer dbmap_audio_classify_microphone. */
    dbmap_audio_mic_class_input_t in;
    memset(&in, 0, sizeof(in));
    in.transport_status = transport_status;
    in.peak_dbfs = peak_dbfs;
    in.rms_dbfs = peak_dbfs;
    if (!(peak_dbfs > DBMAP_AUDIO_SILENCE_DBFS)) {
        in.sample_count = 1;
        in.diag.zero_sample_count = 1;
    } else {
        /* Synthetic ambient-like shape so legacy "peak above floor" still maps to
         * signal_present when callers lack window diagnostics. */
        in.sample_count = 1000;
        in.diag.unique_sample_count = 64;
        in.diag.unique_capped = 1;
        in.diag.consecutive_identical_samples = 1;
        in.diag.zero_sample_count = 0;
    }
    return dbmap_audio_classify_microphone(&in, NULL, 0);
}

int32_t dbmap_inmp441_extract_sample(uint32_t slot_word)
{
    /*
     * INMP441 datasheet: 24-bit I2S audio in a wider slot; we assume left-justified
     * data in bits 31..8 of a 32-bit Philips word (arithmetic >> 8). EAR-001 raw_min
     * of -8388608 (-2^23) is consistent with this model when the slot MSB is set.
     * Board-level alignment is still unverified on a scope — do not change without
     * measurement.
     */
    return (int32_t)slot_word >> 8;
}
