#include "dbmap_audio_metrics.h"

#include <math.h>
#include <stdint.h>

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

int32_t dbmap_inmp441_extract_sample(uint32_t slot_word)
{
    /* UNVERIFIED_ALIGNMENT: left-justified 24-bit in 32-bit Philips slot. */
    return (int32_t)slot_word >> 8;
}
