#include "dbmap_acoustic_l0.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
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

static float power_to_dbfs(double power_rel)
{
    if (!(power_rel > 0.0)) {
        return DBMAP_L0_SILENCE_DBFS;
    }
    double dbfs = 10.0 * log10(power_rel);
    if (dbfs < DBMAP_L0_SILENCE_DBFS) {
        return DBMAP_L0_SILENCE_DBFS;
    }
    if (dbfs > 0.0) {
        return 0.0f;
    }
    return (float)dbfs;
}

static float amp_to_dbfs(double magnitude, double full_scale)
{
    if (!(magnitude > 0.0) || !(full_scale > 0.0)) {
        return DBMAP_L0_SILENCE_DBFS;
    }
    double ratio = magnitude / full_scale;
    if (ratio <= 0.0) {
        return DBMAP_L0_SILENCE_DBFS;
    }
    double dbfs = 20.0 * log10(ratio);
    if (dbfs < DBMAP_L0_SILENCE_DBFS) {
        return DBMAP_L0_SILENCE_DBFS;
    }
    if (dbfs > 0.0) {
        return 0.0f;
    }
    return (float)dbfs;
}

float dbmap_l0_band_low_hz(dbmap_l0_band_id_t band)
{
    static const float lo[DBMAP_L0_BAND_COUNT] = {20.0f, 250.0f, 1000.0f, 4000.0f};
    if ((int)band < 0 || (int)band >= DBMAP_L0_BAND_COUNT) {
        return 0.0f;
    }
    return lo[band];
}

float dbmap_l0_band_high_hz(dbmap_l0_band_id_t band)
{
    static const float hi[DBMAP_L0_BAND_COUNT] = {250.0f, 1000.0f, 4000.0f, 8000.0f};
    if ((int)band < 0 || (int)band >= DBMAP_L0_BAND_COUNT) {
        return 0.0f;
    }
    return hi[band];
}

static void biquad_clear_state(dbmap_l0_biquad_t *bq)
{
    bq->z1 = 0.0f;
    bq->z2 = 0.0f;
}

static float biquad_process(dbmap_l0_biquad_t *bq, float x)
{
    /* Transposed direct form II */
    float y = bq->b0 * x + bq->z1;
    bq->z1 = bq->b1 * x - bq->a1 * y + bq->z2;
    bq->z2 = bq->b2 * x - bq->a2 * y;
    return y;
}

/* RBJ / Audio EQ Cookbook — Butterworth-ish 2nd-order LPF (Q = 1/sqrt(2)). */
static void design_lowpass(dbmap_l0_biquad_t *bq, float fs, float f_c)
{
    if (f_c < 1.0f) {
        f_c = 1.0f;
    }
    if (f_c > fs * 0.49f) {
        f_c = fs * 0.49f;
    }
    const float w0 = 2.0f * (float)M_PI * (f_c / fs);
    const float cosw = cosf(w0);
    const float sinw = sinf(w0);
    const float alpha = sinw / (2.0f * 0.7071067811865476f);
    const float a0 = 1.0f + alpha;
    bq->b0 = ((1.0f - cosw) * 0.5f) / a0;
    bq->b1 = (1.0f - cosw) / a0;
    bq->b2 = ((1.0f - cosw) * 0.5f) / a0;
    bq->a1 = (-2.0f * cosw) / a0;
    bq->a2 = (1.0f - alpha) / a0;
    biquad_clear_state(bq);
}

static void design_highpass(dbmap_l0_biquad_t *bq, float fs, float f_c)
{
    if (f_c < 1.0f) {
        f_c = 1.0f;
    }
    if (f_c > fs * 0.49f) {
        f_c = fs * 0.49f;
    }
    const float w0 = 2.0f * (float)M_PI * (f_c / fs);
    const float cosw = cosf(w0);
    const float sinw = sinf(w0);
    const float alpha = sinw / (2.0f * 0.7071067811865476f);
    const float a0 = 1.0f + alpha;
    bq->b0 = ((1.0f + cosw) * 0.5f) / a0;
    bq->b1 = (-(1.0f + cosw)) / a0;
    bq->b2 = ((1.0f + cosw) * 0.5f) / a0;
    bq->a1 = (-2.0f * cosw) / a0;
    bq->a2 = (1.0f - alpha) / a0;
    biquad_clear_state(bq);
}

void dbmap_l0_filter_bank_init(dbmap_l0_filter_bank_t *bank, uint32_t sample_rate_hz)
{
    if (bank == NULL) {
        return;
    }
    memset(bank, 0, sizeof(*bank));
    if (sample_rate_hz < 1000u) {
        sample_rate_hz = 16000u;
    }
    bank->sample_rate_hz = sample_rate_hz;
    const float fs = (float)sample_rate_hz;
    for (int i = 0; i < DBMAP_L0_BAND_COUNT; ++i) {
        design_highpass(&bank->bands[i].hp, fs, dbmap_l0_band_low_hz((dbmap_l0_band_id_t)i));
        design_lowpass(&bank->bands[i].lp, fs, dbmap_l0_band_high_hz((dbmap_l0_band_id_t)i));
    }
}

void dbmap_l0_filter_bank_reset(dbmap_l0_filter_bank_t *bank)
{
    if (bank == NULL) {
        return;
    }
    for (int i = 0; i < DBMAP_L0_BAND_COUNT; ++i) {
        biquad_clear_state(&bank->bands[i].hp);
        biquad_clear_state(&bank->bands[i].lp);
    }
}

bool dbmap_l0_window_in_interval(
    int64_t window_end_mono_us,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us
)
{
    /*
     * L0 association uses exclusive-start / inclusive-end on window_end:
     *   interval_start < window_end <= interval_end
     * Observation wall/mono span remains [start, end). This partitions
     * chained PERIODIC closes (next.start == prev.end) without gaps or
     * double-counts, and includes the closing window on START/PERIODIC/END.
     */
    return window_end_mono_us > interval_start_mono_us
        && window_end_mono_us <= interval_end_mono_us;
}

void dbmap_l0_analyze_window(
    dbmap_l0_filter_bank_t *bank,
    const int32_t *samples,
    size_t count,
    int full_scale_bits,
    uint32_t window_ms,
    int64_t window_end_mono_us,
    dbmap_l0_window_t *out
)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->window_end_mono_us = window_end_mono_us;
    out->window_ms = window_ms;
    out->rms_dbfs = DBMAP_L0_SILENCE_DBFS;
    out->peak_dbfs = DBMAP_L0_SILENCE_DBFS;
    out->crest_db = 0.0f;
    for (int i = 0; i < DBMAP_L0_BAND_COUNT; ++i) {
        out->band_dbfs[i] = DBMAP_L0_SILENCE_DBFS;
    }

    if (bank == NULL || samples == NULL || count == 0) {
        out->valid = false;
        return;
    }

    dbmap_l0_filter_bank_reset(bank);

    const double fs_mag = full_scale_magnitude(full_scale_bits);
    const double inv_fs = 1.0 / fs_mag;
    double sum_sq = 0.0;
    int32_t peak_abs = 0;
    uint32_t clipped = 0;
    const int32_t clip_level = (int32_t)(fs_mag - 1.0);
    double band_sum_sq[DBMAP_L0_BAND_COUNT];
    memset(band_sum_sq, 0, sizeof(band_sum_sq));

    for (size_t n = 0; n < count; ++n) {
        int32_t s = samples[n];
        int32_t a = s;
        if (a < 0) {
            a = (a == INT32_MIN) ? INT32_MAX : -a;
        }
        if (a > peak_abs) {
            peak_abs = a;
        }
        if (a >= clip_level) {
            clipped++;
        }
        double xn = (double)s * inv_fs;
        sum_sq += xn * xn;
        float xf = (float)xn;
        for (int b = 0; b < DBMAP_L0_BAND_COUNT; ++b) {
            float y = biquad_process(&bank->bands[b].hp, xf);
            y = biquad_process(&bank->bands[b].lp, y);
            band_sum_sq[b] += (double)y * (double)y;
        }
    }

    out->sample_count = (uint32_t)count;
    out->clipped_sample_count = clipped;
    out->valid = true;
    out->rms_dbfs = amp_to_dbfs(sqrt(sum_sq / (double)count), 1.0);
    out->peak_dbfs = amp_to_dbfs((double)peak_abs * inv_fs, 1.0);
    out->crest_db = out->peak_dbfs - out->rms_dbfs;
    out->clipped = (clipped > 0) || (out->peak_dbfs >= DBMAP_L0_CLIP_PEAK_DBFS);

    double e_low = 0.0;
    double e_high = 0.0;
    for (int b = 0; b < DBMAP_L0_BAND_COUNT; ++b) {
        double e = band_sum_sq[b] / (double)count; /* mean-square vs FS^2 */
        out->band_energy_linear[b] = e;
        out->band_dbfs[b] = power_to_dbfs(e);
        if (b <= DBMAP_L0_BAND_B1) {
            e_low += e;
        } else {
            e_high += e;
        }
    }

    if (e_low > DBMAP_L0_VALID_ENERGY_FLOOR && e_high > 0.0) {
        out->hl_valid = true;
        out->hl_db = (float)(10.0 * log10(e_high / e_low));
    } else if (e_high > DBMAP_L0_VALID_ENERGY_FLOOR && e_low > 0.0) {
        out->hl_valid = true;
        out->hl_db = (float)(10.0 * log10(e_high / e_low));
    } else {
        out->hl_valid = false;
        out->hl_db = 0.0f;
    }
}

void dbmap_l0_interval_accum_init(
    dbmap_l0_interval_accum_t *acc,
    uint32_t sample_rate_hz,
    uint32_t analysis_window_ms,
    uint32_t mic_index,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    float *hist_rms_dbfs,
    float *hist_hl_db,
    uint8_t *hist_hl_valid,
    int64_t *hist_t_end_us,
    float *hist_crest_db,
    uint8_t *hist_crest_ok,
    uint32_t hist_cap
)
{
    if (acc == NULL) {
        return;
    }
    memset(acc, 0, sizeof(*acc));
    acc->sample_rate_hz = sample_rate_hz;
    acc->analysis_window_ms = analysis_window_ms;
    acc->mic_index = mic_index;
    acc->interval_start_mono_us = interval_start_mono_us;
    acc->interval_end_mono_us = interval_end_mono_us;
    acc->hist_rms_dbfs = hist_rms_dbfs;
    acc->hist_hl_db = hist_hl_db;
    acc->hist_hl_valid = hist_hl_valid;
    acc->hist_t_end_us = hist_t_end_us;
    acc->hist_crest_db = hist_crest_db;
    acc->hist_crest_ok = hist_crest_ok;
    acc->hist_cap = hist_cap;
    acc->rms_max_dbfs = DBMAP_L0_SILENCE_DBFS;
    acc->rms_min_dbfs = 0.0f;
}

void dbmap_l0_interval_accum_add(
    dbmap_l0_interval_accum_t *acc,
    const dbmap_l0_window_t *window
)
{
    if (acc == NULL || window == NULL || !window->valid) {
        return;
    }
    if (!dbmap_l0_window_in_interval(
            window->window_end_mono_us,
            acc->interval_start_mono_us,
            acc->interval_end_mono_us
        )) {
        return;
    }

    if (acc->window_count == 0) {
        acc->rms_max_dbfs = window->rms_dbfs;
        acc->rms_min_dbfs = window->rms_dbfs;
    } else {
        if (window->rms_dbfs > acc->rms_max_dbfs) {
            acc->rms_max_dbfs = window->rms_dbfs;
        }
        if (window->rms_dbfs < acc->rms_min_dbfs) {
            acc->rms_min_dbfs = window->rms_dbfs;
        }
    }

    acc->sum_rms_dbfs += (double)window->rms_dbfs;
    for (int b = 0; b < DBMAP_L0_BAND_COUNT; ++b) {
        acc->sum_band_energy[b] += window->band_energy_linear[b];
    }

    if (window->clipped) {
        acc->clipped_window_count += 1;
    }

    const bool crest_ok = !window->clipped;
    if (crest_ok) {
        acc->n_crest += 1;
    }

    if (acc->hist_rms_dbfs == NULL || acc->hist_cap == 0) {
        acc->history_overflow = true;
    } else if (acc->hist_count >= acc->hist_cap) {
        /* Never silently drop history while still counting the window for means. */
        acc->history_overflow = true;
    } else {
        const uint32_t i = acc->hist_count;
        acc->hist_rms_dbfs[i] = window->rms_dbfs;
        if (acc->hist_t_end_us) {
            acc->hist_t_end_us[i] = window->window_end_mono_us;
        }
        if (acc->hist_hl_db && acc->hist_hl_valid) {
            acc->hist_hl_db[i] = window->hl_db;
            acc->hist_hl_valid[i] = window->hl_valid ? 1u : 0u;
        }
        if (acc->hist_crest_db && acc->hist_crest_ok) {
            acc->hist_crest_db[i] = window->crest_db;
            acc->hist_crest_ok[i] = crest_ok ? 1u : 0u;
        }
        acc->hist_count += 1;
    }

    acc->window_count += 1;
}

static bool ols_slope(
    const float *y,
    const int64_t *t_us,
    const uint8_t *y_ok,
    uint32_t n,
    float *slope_per_s_out
)
{
    if (slope_per_s_out == NULL || y == NULL || t_us == NULL || n < 3) {
        return false;
    }
    double sum_t = 0.0;
    double sum_y = 0.0;
    double sum_tt = 0.0;
    double sum_ty = 0.0;
    uint32_t m = 0;
    const double t0 = (double)t_us[0];
    for (uint32_t i = 0; i < n; ++i) {
        if (y_ok != NULL && !y_ok[i]) {
            continue;
        }
        const double t = ((double)t_us[i] - t0) * 1.0e-6;
        const double yi = (double)y[i];
        sum_t += t;
        sum_y += yi;
        sum_tt += t * t;
        sum_ty += t * yi;
        m += 1;
    }
    if (m < 3) {
        return false;
    }
    const double denom = (double)m * sum_tt - sum_t * sum_t;
    if (!(fabs(denom) > 1.0e-18)) {
        return false;
    }
    *slope_per_s_out = (float)(((double)m * sum_ty - sum_t * sum_y) / denom);
    return true;
}

void dbmap_l0_interval_accum_finalize(
    const dbmap_l0_interval_accum_t *acc,
    dbmap_l0_interval_t *out
)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (acc == NULL) {
        return;
    }

    out->window_count = acc->window_count;
    out->clipped_window_count = acc->clipped_window_count;
    out->analysis_window_ms = acc->analysis_window_ms;
    out->sample_rate_hz = acc->sample_rate_hz;
    out->mic_index = acc->mic_index;
    out->interval_start_mono_us = acc->interval_start_mono_us;
    out->interval_end_mono_us = acc->interval_end_mono_us;
    out->n_crest = acc->n_crest;
    out->history_overflow = acc->history_overflow;

    if (acc->window_count == 0) {
        return;
    }

    out->has_rms_stats = true;
    out->rms_mean_dbfs = (float)(acc->sum_rms_dbfs / (double)acc->window_count);
    out->rms_max_dbfs = acc->rms_max_dbfs;
    out->rms_min_dbfs = acc->rms_min_dbfs;

    double e_tot = 0.0;
    for (int b = 0; b < DBMAP_L0_BAND_COUNT; ++b) {
        const double mean_e = acc->sum_band_energy[b] / (double)acc->window_count;
        out->band_mean_dbfs[b] = power_to_dbfs(mean_e);
        e_tot += acc->sum_band_energy[b];
    }
    out->has_band_means = true;

    if (e_tot > DBMAP_L0_VALID_ENERGY_FLOOR * (double)acc->window_count) {
        out->has_band_fractions = true;
        for (int b = 0; b < DBMAP_L0_BAND_COUNT; ++b) {
            out->band_fraction[b] = (float)(acc->sum_band_energy[b] / e_tot);
        }
    }

    /* Interval H/L mean from Σ linear energies (not average of per-window HL_db). */
    {
        const double e_low =
            acc->sum_band_energy[DBMAP_L0_BAND_B0] + acc->sum_band_energy[DBMAP_L0_BAND_B1];
        const double e_high =
            acc->sum_band_energy[DBMAP_L0_BAND_B2] + acc->sum_band_energy[DBMAP_L0_BAND_B3];
        const double floor_n =
            DBMAP_L0_VALID_ENERGY_FLOOR * (double)acc->window_count;
        if (e_low > floor_n && e_high > 0.0) {
            out->has_hl_mean = true;
            out->hl_mean_db = (float)(10.0 * log10(e_high / e_low));
        } else if (e_high > floor_n && e_low > 0.0) {
            out->has_hl_mean = true;
            out->hl_mean_db = (float)(10.0 * log10(e_high / e_low));
        } else {
            out->has_hl_mean = false;
            out->hl_mean_db = 0.0f;
        }
    }

    /* Temporal features require complete history — never emit partial T* after overflow. */
    if (acc->history_overflow) {
        out->has_t1 = false;
        out->has_t2 = false;
        out->has_t3 = false;
        out->has_t6 = false;
        out->rms_stdev_db = 0.0f;
        out->rms_range_db = out->rms_max_dbfs - out->rms_min_dbfs;
        return;
    }

    /* T1: range from min/max; sample stdev when history has ≥2 points. */
    out->rms_range_db = out->rms_max_dbfs - out->rms_min_dbfs;
    if (acc->window_count >= 2) {
        out->has_t1 = true;
        if (acc->hist_count >= 2 && acc->hist_rms_dbfs) {
            double mean = 0.0;
            for (uint32_t i = 0; i < acc->hist_count; ++i) {
                mean += (double)acc->hist_rms_dbfs[i];
            }
            mean /= (double)acc->hist_count;
            double ss = 0.0;
            for (uint32_t i = 0; i < acc->hist_count; ++i) {
                const double d = (double)acc->hist_rms_dbfs[i] - mean;
                ss += d * d;
            }
            out->rms_stdev_db = (float)sqrt(ss / (double)(acc->hist_count - 1u));
        } else {
            out->rms_stdev_db = out->rms_range_db; /* degraded: no history buffer */
        }
    }

    /* T2 */
    if (acc->hist_count >= 3 && acc->hist_rms_dbfs && acc->hist_t_end_us) {
        float slope = 0.0f;
        if (ols_slope(acc->hist_rms_dbfs, acc->hist_t_end_us, NULL, acc->hist_count, &slope)) {
            out->has_t2 = true;
            out->rms_slope_db_per_s = slope;
        }
    }

    /* T3 */
    if (acc->n_crest == 0 || acc->hist_crest_db == NULL || acc->hist_crest_ok == NULL) {
        out->has_t3 = false;
    } else {
        bool any = false;
        float crest_max = -1000.0f;
        uint32_t high = 0;
        for (uint32_t i = 0; i < acc->hist_count; ++i) {
            if (!acc->hist_crest_ok[i]) {
                continue;
            }
            const float c = acc->hist_crest_db[i];
            if (!any || c > crest_max) {
                crest_max = c;
                any = true;
            }
            if (c >= DBMAP_L0_CREST_THRESHOLD_DB) {
                high += 1;
            }
        }
        if (any && acc->n_crest > 0) {
            out->has_t3 = true;
            out->crest_max_db = crest_max;
            out->high_crest_count = high;
            out->high_crest_rate = (float)high / (float)acc->n_crest;
        }
    }

    /* T6 */
    if (acc->hist_count >= 3 && acc->hist_hl_db && acc->hist_hl_valid && acc->hist_t_end_us) {
        float slope = 0.0f;
        if (ols_slope(
                acc->hist_hl_db,
                acc->hist_t_end_us,
                acc->hist_hl_valid,
                acc->hist_count,
                &slope
            )) {
            out->has_t6 = true;
            out->hl_slope_db_per_s = slope;
        }
    }
}
