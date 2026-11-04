#include "dbmap_acoustic_l2.h"

#include <math.h>
#include <string.h>

void dbmap_l2_continuity_reset(dbmap_l2_continuity_t *cache)
{
    if (cache == NULL) {
        return;
    }
    memset(cache, 0, sizeof(*cache));
}

void dbmap_l2_continuity_push(
    dbmap_l2_continuity_t *cache,
    uint32_t analysis_window_ms,
    dbmap_l2_window_t *window
)
{
    if (window == NULL) {
        return;
    }
    window->has_dominant_delta = false;
    window->dominant_delta_hz = 0.0f;

    if (cache == NULL) {
        return;
    }

    if (!window->eligible) {
        dbmap_l2_continuity_reset(cache);
        return;
    }

    if (cache->has_prev) {
        const int64_t tw_us = (int64_t)analysis_window_ms * 1000LL;
        const int64_t dt = window->window_end_mono_us - cache->prev_window_end_us;
        const int64_t err = dt - tw_us;
        const int64_t abs_err = err < 0 ? -err : err;
        if (abs_err <= DBMAP_L2_CONTINUITY_TOL_US) {
            window->has_dominant_delta = true;
            window->dominant_delta_hz =
                fabsf(window->dominant_hz - cache->prev_dominant_hz);
        } else {
            /* Timestamp gap: break then install current as new predecessor. */
            dbmap_l2_continuity_reset(cache);
        }
    }

    cache->has_prev = true;
    cache->prev_dominant_hz = window->dominant_hz;
    cache->prev_window_end_us = window->window_end_mono_us;
}

void dbmap_l2_interval_accum_init(
    dbmap_l2_interval_accum_t *acc,
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

void dbmap_l2_interval_accum_mark_overflow(dbmap_l2_interval_accum_t *acc)
{
    if (acc == NULL) {
        return;
    }
    acc->history_overflow = true;
    acc->window_count = 0;
    acc->invalid_window_count = 0;
    acc->flux_n = 0;
    acc->sum_flux = 0.0;
    acc->sum_flux_sq = 0.0;
    acc->env_n = 0;
    acc->sum_env = 0.0;
    acc->sum_env_sq = 0.0;
    acc->delta_n = 0;
    acc->sum_delta = 0.0;
    acc->sum_delta_sq = 0.0;
}

void dbmap_l2_interval_accum_add(
    dbmap_l2_interval_accum_t *acc,
    const dbmap_l2_window_t *window
)
{
    if (acc == NULL || window == NULL || acc->history_overflow) {
        return;
    }

    const int64_t t = window->window_end_mono_us;
    if (!(acc->interval_start_mono_us < t && t <= acc->interval_end_mono_us)) {
        return;
    }

    if (!window->eligible) {
        acc->invalid_window_count++;
        return;
    }

    acc->window_count++;

    if (window->has_spectral_flux) {
        const double x = (double)window->spectral_flux;
        acc->flux_n++;
        acc->sum_flux += x;
        acc->sum_flux_sq += x * x;
    }
    if (window->has_envelope_cv) {
        const double x = (double)window->envelope_cv;
        acc->env_n++;
        acc->sum_env += x;
        acc->sum_env_sq += x * x;
    }
    if (window->has_dominant_delta) {
        const double x = (double)window->dominant_delta_hz;
        acc->delta_n++;
        acc->sum_delta += x;
        acc->sum_delta_sq += x * x;
    }
}

static void set_feature_stats(
    uint32_t n,
    double sum,
    double sum_sq,
    uint32_t *valid_count,
    bool *has_mean,
    float *mean,
    bool *has_stdev,
    float *stdev
)
{
    *valid_count = n;
    *has_mean = false;
    *has_stdev = false;
    *mean = 0.0f;
    *stdev = 0.0f;
    if (n == 0) {
        return;
    }
    const double mu = sum / (double)n;
    *has_mean = true;
    *mean = (float)mu;
    if (n >= 2u) {
        double var = (sum_sq / (double)n) - mu * mu;
        if (var < 0.0) {
            var = 0.0;
        }
        *has_stdev = true;
        *stdev = (float)sqrt(var);
    }
}

void dbmap_l2_interval_set_empty(
    dbmap_l2_interval_t *out,
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
    out->frame_hop_samples = DBMAP_L1_FFT_SIZE;
    out->interval_start_mono_us = interval_start_mono_us;
    out->interval_end_mono_us = interval_end_mono_us;
}

void dbmap_l2_interval_accum_finalize(
    const dbmap_l2_interval_accum_t *acc,
    dbmap_l2_interval_t *out
)
{
    if (out == NULL) {
        return;
    }
    if (acc == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }

    dbmap_l2_interval_set_empty(
        out,
        acc->sample_rate_hz,
        acc->analysis_window_ms,
        acc->interval_start_mono_us,
        acc->interval_end_mono_us
    );

    if (acc->history_overflow) {
        /* Option A: all counts zero; features cleared; flag true. */
        out->history_overflow = true;
        return;
    }

    out->window_count = acc->window_count;
    out->invalid_window_count = acc->invalid_window_count;
    out->history_overflow = false;

    set_feature_stats(
        acc->flux_n,
        acc->sum_flux,
        acc->sum_flux_sq,
        &out->spectral_flux_valid_count,
        &out->has_spectral_flux_mean,
        &out->spectral_flux_mean,
        &out->has_spectral_flux_stdev,
        &out->spectral_flux_stdev
    );
    set_feature_stats(
        acc->env_n,
        acc->sum_env,
        acc->sum_env_sq,
        &out->envelope_cv_valid_count,
        &out->has_envelope_cv_mean,
        &out->envelope_cv_mean,
        &out->has_envelope_cv_stdev,
        &out->envelope_cv_stdev
    );
    set_feature_stats(
        acc->delta_n,
        acc->sum_delta,
        acc->sum_delta_sq,
        &out->dominant_delta_valid_count,
        &out->has_dominant_delta_mean,
        &out->dominant_delta_mean_hz,
        &out->has_dominant_delta_stdev,
        &out->dominant_delta_stdev_hz
    );
}

void dbmap_l2_analyze_window(
    dbmap_l1_workspace_t *ws,
    const int32_t *samples,
    size_t count,
    int full_scale_bits,
    uint32_t sample_rate_hz,
    uint32_t window_ms,
    int64_t window_end_mono_us,
    dbmap_l1_window_t *l1_out,
    dbmap_l2_window_t *l2_out
)
{
    dbmap_l1_window_t l1_local;
    dbmap_l1_window_t *l1 = (l1_out != NULL) ? l1_out : &l1_local;
    dbmap_l2_intra_metrics_t intra;

    dbmap_l1_analyze_window_ex(
        ws,
        samples,
        count,
        full_scale_bits,
        sample_rate_hz,
        window_ms,
        window_end_mono_us,
        l1,
        &intra
    );

    if (l2_out == NULL) {
        return;
    }
    memset(l2_out, 0, sizeof(*l2_out));
    l2_out->clipped = l1->clipped;
    l2_out->l1_valid = l1->valid;
    l2_out->window_end_mono_us = l1->window_end_mono_us;
    l2_out->window_ms = l1->window_ms;
    l2_out->sample_count = l1->sample_count;
    l2_out->frame_count = l1->frame_count;
    l2_out->dominant_hz = l1->dominant_hz;
    l2_out->eligible =
        l1->valid && !l1->clipped && l1->frame_count >= 2u;

    if (!l2_out->eligible) {
        return;
    }

    if (intra.has_spectral_flux) {
        l2_out->has_spectral_flux = true;
        l2_out->spectral_flux = intra.spectral_flux;
    }
    if (intra.has_envelope_cv) {
        l2_out->has_envelope_cv = true;
        l2_out->envelope_cv = intra.envelope_cv;
    }
}
