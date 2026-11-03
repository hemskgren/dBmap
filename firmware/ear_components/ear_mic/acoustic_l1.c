#include "dbmap_acoustic_l1.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float full_scale_magnitude_f(int full_scale_bits)
{
    if (full_scale_bits < 2) {
        full_scale_bits = 2;
    }
    if (full_scale_bits > 31) {
        full_scale_bits = 31;
    }
    return (float)(1LL << (full_scale_bits - 1));
}

static float amp_to_dbfs_f(float magnitude, float full_scale)
{
    if (!(magnitude > 0.0f) || !(full_scale > 0.0f)) {
        return DBMAP_L1_SILENCE_DBFS;
    }
    float ratio = magnitude / full_scale;
    if (!(ratio > 0.0f)) {
        return DBMAP_L1_SILENCE_DBFS;
    }
    float dbfs = 20.0f * log10f(ratio);
    if (dbfs < DBMAP_L1_SILENCE_DBFS) {
        return DBMAP_L1_SILENCE_DBFS;
    }
    if (dbfs > 0.0f) {
        return 0.0f;
    }
    return dbfs;
}

void dbmap_l1_workspace_init(dbmap_l1_workspace_t *ws)
{
    if (ws == NULL) {
        return;
    }
    memset(ws, 0, sizeof(*ws));
    for (uint32_t n = 0; n < DBMAP_L1_FFT_SIZE; n++) {
        ws->hann[n] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)n / (float)(DBMAP_L1_FFT_SIZE - 1)));
    }

    /* Bit-reversal permutation for N=1024. */
    for (uint32_t i = 0; i < DBMAP_L1_FFT_SIZE; i++) {
        uint32_t x = i;
        uint32_t r = 0;
        for (uint32_t b = 0; b < 10u; b++) {
            r = (r << 1) | (x & 1u);
            x >>= 1;
        }
        ws->bitrev[i] = (uint16_t)r;
    }

    /* Twiddles exp(-2π i k / N) for k = 0 .. N/2-1. */
    for (uint32_t k = 0; k < (DBMAP_L1_FFT_SIZE / 2u); k++) {
        const float ang = -2.0f * (float)M_PI * (float)k / (float)DBMAP_L1_FFT_SIZE;
        ws->tw_re[k] = cosf(ang);
        ws->tw_im[k] = sinf(ang);
    }

    ws->tables_ready = true;
}

/* In-place radix-2 Cooley–Tukey using precomputed bitrev + twiddles. */
static void fft_radix2(dbmap_l1_workspace_t *ws)
{
    float *re = ws->time_re;
    float *im = ws->time_im;
    const uint32_t n = DBMAP_L1_FFT_SIZE;

    for (uint32_t i = 0; i < n; i++) {
        const uint32_t j = ws->bitrev[i];
        if (i < j) {
            float tr = re[i];
            re[i] = re[j];
            re[j] = tr;
            float ti = im[i];
            im[i] = im[j];
            im[j] = ti;
        }
    }

    for (uint32_t len = 2; len <= n; len <<= 1) {
        const uint32_t half = len >> 1;
        const uint32_t step = n / len;
        for (uint32_t i = 0; i < n; i += len) {
            for (uint32_t j = 0; j < half; j++) {
                const uint32_t tidx = j * step;
                const float w_re = ws->tw_re[tidx];
                const float w_im = ws->tw_im[tidx];
                const uint32_t u = i + j;
                const uint32_t v = u + half;
                const float t_re = w_re * re[v] - w_im * im[v];
                const float t_im = w_re * im[v] + w_im * re[v];
                re[v] = re[u] - t_re;
                im[v] = im[u] - t_im;
                re[u] += t_re;
                im[u] += t_im;
            }
        }
    }
}

static void features_from_power(
    const float *power,
    uint32_t sample_rate_hz,
    float *centroid_hz,
    float *bandwidth_hz,
    float *dominant_hz,
    float *flatness,
    bool *ok
)
{
    *ok = false;
    *centroid_hz = 0.0f;
    *bandwidth_hz = 0.0f;
    *dominant_hz = 0.0f;
    *flatness = 0.0f;

    const float bin_hz = (float)sample_rate_hz / (float)DBMAP_L1_FFT_SIZE;
    float sum_power = 0.0f;
    float sum_mag = 0.0f;
    float sum_f_mag = 0.0f;
    float log_mag_sum = 0.0f;
    uint32_t mag_bins = 0;
    float peak_power = -1.0f;
    uint32_t peak_k = 0;

    for (uint32_t k = 0; k < DBMAP_L1_FFT_BINS; k++) {
        const float f = bin_hz * (float)k;
        if (f < DBMAP_L1_FREQ_MIN_HZ || f > DBMAP_L1_FREQ_MAX_HZ) {
            continue;
        }
        const float p = power[k];
        if (!(p >= 0.0f) || !isfinite(p)) {
            continue;
        }
        sum_power += p;
        const float mag = sqrtf(p);
        sum_mag += mag;
        sum_f_mag += f * mag;
        if (mag > 1.0e-30f) {
            log_mag_sum += logf(mag);
            mag_bins++;
        }
        if (p > peak_power) {
            peak_power = p;
            peak_k = k;
        }
    }

    if (!(sum_power > (float)DBMAP_L1_VALID_ENERGY_FLOOR) || !(sum_mag > 0.0f) || mag_bins == 0) {
        return;
    }

    const float centroid = sum_f_mag / sum_mag;
    float sum_var = 0.0f;
    for (uint32_t k = 0; k < DBMAP_L1_FFT_BINS; k++) {
        const float f = bin_hz * (float)k;
        if (f < DBMAP_L1_FREQ_MIN_HZ || f > DBMAP_L1_FREQ_MAX_HZ) {
            continue;
        }
        const float mag = sqrtf(power[k]);
        const float df = f - centroid;
        sum_var += df * df * mag;
    }
    const float bandwidth = sqrtf(sum_var / sum_mag);
    const float dominant = bin_hz * (float)peak_k;
    const float geo = expf(log_mag_sum / (float)mag_bins);
    const float arith = sum_mag / (float)mag_bins;
    float flat = 0.0f;
    if (arith > 0.0f) {
        flat = geo / arith;
        if (flat < 0.0f) {
            flat = 0.0f;
        }
        if (flat > 1.0f) {
            flat = 1.0f;
        }
    }

    *centroid_hz = centroid;
    *bandwidth_hz = bandwidth;
    *dominant_hz = dominant;
    *flatness = flat;
    *ok = true;
}

void dbmap_l1_analyze_window(
    dbmap_l1_workspace_t *ws,
    const int32_t *samples,
    size_t count,
    int full_scale_bits,
    uint32_t sample_rate_hz,
    uint32_t window_ms,
    int64_t window_end_mono_us,
    dbmap_l1_window_t *out
)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->window_end_mono_us = window_end_mono_us;
    out->window_ms = window_ms;
    out->sample_count = (uint32_t)count;

    if (ws == NULL || samples == NULL || count == 0 || sample_rate_hz == 0) {
        return;
    }
    if (!ws->tables_ready) {
        dbmap_l1_workspace_init(ws);
    }

    const float full_scale = full_scale_magnitude_f(full_scale_bits);
    const float inv_fs = (full_scale > 0.0f) ? (1.0f / full_scale) : 0.0f;

    /* Peak/clip via integer abs — avoids soft-double and log in the scan. */
    int32_t peak_abs = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t s = samples[i];
        int32_t a = s < 0 ? (s == INT32_MIN ? INT32_MAX : -s) : s;
        if (a > peak_abs) {
            peak_abs = a;
        }
    }
    const float peak_dbfs = amp_to_dbfs_f((float)peak_abs, full_scale);
    out->clipped = (peak_dbfs >= DBMAP_L1_CLIP_PEAK_DBFS);

    if (count < DBMAP_L1_FFT_SIZE) {
        return;
    }

    memset(ws->power_acc, 0, sizeof(ws->power_acc));
    uint32_t frames = 0;
    for (size_t offset = 0; offset + DBMAP_L1_FFT_SIZE <= count; offset += DBMAP_L1_FFT_SIZE) {
        const float *hann = ws->hann;
        float *re = ws->time_re;
        float *im = ws->time_im;
        const int32_t *frame = samples + offset;
        for (uint32_t n = 0; n < DBMAP_L1_FFT_SIZE; n++) {
            /* Normalize to full-scale units before Hann so power is FS-relative. */
            re[n] = ((float)frame[n] * inv_fs) * hann[n];
            im[n] = 0.0f;
        }
        fft_radix2(ws);
        for (uint32_t k = 0; k < DBMAP_L1_FFT_BINS; k++) {
            const float rr = re[k];
            const float ii = im[k];
            ws->power_acc[k] += rr * rr + ii * ii;
        }
        frames++;
    }

    out->frame_count = frames;
    if (frames == 0) {
        return;
    }

    const float inv_frames = 1.0f / (float)frames;
    for (uint32_t k = 0; k < DBMAP_L1_FFT_BINS; k++) {
        ws->power_acc[k] *= inv_frames;
    }

    bool ok = false;
    features_from_power(
        ws->power_acc,
        sample_rate_hz,
        &out->centroid_hz,
        &out->bandwidth_hz,
        &out->dominant_hz,
        &out->flatness,
        &ok
    );
    out->valid = ok;
}

void dbmap_l1_interval_accum_init(
    dbmap_l1_interval_accum_t *acc,
    uint32_t sample_rate_hz,
    uint32_t analysis_window_ms,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us
)
{
    if (acc == NULL) {
        return;
    }
    memset(acc, 0, sizeof(*acc));
    acc->sample_rate_hz = sample_rate_hz;
    acc->analysis_window_ms = analysis_window_ms;
    acc->interval_start_mono_us = interval_start_mono_us;
    acc->interval_end_mono_us = interval_end_mono_us;
}

void dbmap_l1_interval_accum_add(
    dbmap_l1_interval_accum_t *acc,
    const dbmap_l1_window_t *window
)
{
    if (acc == NULL || window == NULL) {
        return;
    }
    if (!(acc->interval_start_mono_us < window->window_end_mono_us &&
          window->window_end_mono_us <= acc->interval_end_mono_us)) {
        return;
    }
    if (!window->valid || window->clipped) {
        acc->invalid_window_count++;
        return;
    }
    acc->window_count++;
    acc->sum_centroid += (double)window->centroid_hz;
    acc->sum_bandwidth += (double)window->bandwidth_hz;
    acc->sum_dominant += (double)window->dominant_hz;
    acc->sum_flatness += (double)window->flatness;
    acc->sum_centroid_sq += (double)window->centroid_hz * (double)window->centroid_hz;
    acc->sum_bandwidth_sq += (double)window->bandwidth_hz * (double)window->bandwidth_hz;
    acc->sum_dominant_sq += (double)window->dominant_hz * (double)window->dominant_hz;
    acc->sum_flatness_sq += (double)window->flatness * (double)window->flatness;
}

static float sample_stdev(double sum, double sum_sq, uint32_t n)
{
    if (n < 2) {
        return 0.0f;
    }
    const double mean = sum / (double)n;
    double var = (sum_sq / (double)n) - mean * mean;
    if (var < 0.0) {
        var = 0.0;
    }
    /* Population stdev over associated windows (stable for small n). */
    return (float)sqrt(var);
}

void dbmap_l1_interval_accum_finalize(
    const dbmap_l1_interval_accum_t *acc,
    dbmap_l1_interval_t *out
)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (acc == NULL) {
        return;
    }
    out->sample_rate_hz = acc->sample_rate_hz;
    out->analysis_window_ms = acc->analysis_window_ms;
    out->fft_size = DBMAP_L1_FFT_SIZE;
    out->freq_min_hz = DBMAP_L1_FREQ_MIN_HZ;
    out->freq_max_hz = DBMAP_L1_FREQ_MAX_HZ;
    out->interval_start_mono_us = acc->interval_start_mono_us;
    out->interval_end_mono_us = acc->interval_end_mono_us;
    out->window_count = acc->window_count;
    out->invalid_window_count = acc->invalid_window_count;
    out->has_stdev = (acc->window_count >= 2);

    if (acc->window_count == 0) {
        return;
    }

    out->has_centroid = true;
    out->centroid_mean_hz = (float)(acc->sum_centroid / (double)acc->window_count);
    out->centroid_stdev_hz = sample_stdev(acc->sum_centroid, acc->sum_centroid_sq, acc->window_count);

    out->has_bandwidth = true;
    out->bandwidth_mean_hz = (float)(acc->sum_bandwidth / (double)acc->window_count);
    out->bandwidth_stdev_hz = sample_stdev(acc->sum_bandwidth, acc->sum_bandwidth_sq, acc->window_count);

    out->has_dominant = true;
    out->dominant_mean_hz = (float)(acc->sum_dominant / (double)acc->window_count);
    out->dominant_stdev_hz = sample_stdev(acc->sum_dominant, acc->sum_dominant_sq, acc->window_count);

    out->has_flatness = true;
    out->flatness_mean = (float)(acc->sum_flatness / (double)acc->window_count);
    out->flatness_stdev = sample_stdev(acc->sum_flatness, acc->sum_flatness_sq, acc->window_count);
}

void dbmap_l1_interval_set_empty(
    dbmap_l1_interval_t *out,
    uint32_t sample_rate_hz,
    uint32_t analysis_window_ms,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us
)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->sample_rate_hz = sample_rate_hz;
    out->analysis_window_ms = analysis_window_ms;
    out->fft_size = DBMAP_L1_FFT_SIZE;
    out->freq_min_hz = DBMAP_L1_FREQ_MIN_HZ;
    out->freq_max_hz = DBMAP_L1_FREQ_MAX_HZ;
    out->interval_start_mono_us = interval_start_mono_us;
    out->interval_end_mono_us = interval_end_mono_us;
}
