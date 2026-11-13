#include "dbmap_ear_acoustic.h"

#include <string.h>

#include "dbmap_acoustic_l0.h"
#include "dbmap_acoustic_l2.h"
#include "sp_mic_pcm.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

static const char *TAG = "ear_acoustic";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

#if CONFIG_DBMAP_EAR_MIC_L0
static dbmap_l0_filter_bank_t s_l0_bank;
static dbmap_l0_window_t s_l0_latest;
static bool s_l0_latest_ready;
static bool s_l0_bank_ready;
static uint32_t s_l0_analyze_count;
static uint32_t s_l0_analyze_us_last;
static uint32_t s_l0_analyze_us_max;
#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
static dbmap_l1_workspace_t s_l1_ws;
static dbmap_l1_window_t s_l1_latest;
static bool s_l1_latest_ready;
static bool s_l1_ws_ready;
static uint32_t s_l1_analyze_count;
static uint32_t s_l1_analyze_us_last;
static uint32_t s_l1_analyze_us_max;
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
static dbmap_l2_window_t s_l2_latest;
static bool s_l2_latest_ready;
static uint32_t s_l2_analyze_count;
static uint32_t s_l2_combined_us_last;
static uint32_t s_l2_combined_us_max;
static uint32_t s_l2_incr_est_us_last;
static uint32_t s_l2_incr_est_us_max;
static uint32_t s_l2_rms_us_last;
static uint32_t s_l2_rms_us_max;
#endif
#endif
#endif

#if CONFIG_DBMAP_EAR_MIC_L0
static void pcm_ready_hook(uint32_t sample_rate_hz)
{
    dbmap_l0_filter_bank_init(&s_l0_bank, sample_rate_hz);
    memset(&s_l0_latest, 0, sizeof(s_l0_latest));
    s_l0_latest_ready = false;
    s_l0_bank_ready = true;
    s_l0_analyze_count = 0;
    s_l0_analyze_us_last = 0;
    s_l0_analyze_us_max = 0;
    ESP_LOGI(
        TAG,
        "L0 ready sample_rate=%lu (filter bank init after PCM alloc)",
        (unsigned long)sample_rate_hz
    );
#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
    dbmap_l1_workspace_init(&s_l1_ws);
    memset(&s_l1_latest, 0, sizeof(s_l1_latest));
    s_l1_latest_ready = false;
    s_l1_ws_ready = true;
    s_l1_analyze_count = 0;
    s_l1_analyze_us_last = 0;
    s_l1_analyze_us_max = 0;
    ESP_LOGI(
        TAG,
        "L1 ready fft=%u band=%.0f-%.0f Hz (static workspace float+twiddle LUT)",
        (unsigned)DBMAP_L1_FFT_SIZE,
        (double)DBMAP_L1_FREQ_MIN_HZ,
        (double)DBMAP_L1_FREQ_MAX_HZ
    );
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
    memset(&s_l2_latest, 0, sizeof(s_l2_latest));
    s_l2_latest_ready = false;
    s_l2_analyze_count = 0;
    s_l2_combined_us_last = 0;
    s_l2_combined_us_max = 0;
    s_l2_incr_est_us_last = 0;
    s_l2_incr_est_us_max = 0;
    s_l2_rms_us_last = 0;
    s_l2_rms_us_max = 0;
    ESP_LOGI(
        TAG,
        "L2 ready compute=1 publish=%d (fused L1+L2; dynamics MQTT off unless PUBLISH)",
#if defined(CONFIG_DBMAP_EAR_OBS_L2_PUBLISH) && CONFIG_DBMAP_EAR_OBS_L2_PUBLISH
        1
#else
        0
#endif
    );
#endif
#endif
}
#endif

esp_err_t dbmap_ear_acoustic_register(void)
{
#if !CONFIG_DBMAP_EAR_MIC_L0
    return ESP_OK;
#else
    sp_mic_set_pcm_ready_hook(pcm_ready_hook);
    return ESP_OK;
#endif
}

esp_err_t dbmap_ear_acoustic_analyze_pcm_job(const sp_mic_pcm_job_t *job)
{
#if !CONFIG_DBMAP_EAR_MIC_L0
    (void)job;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (job == NULL || job->samples == NULL || job->sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sp_mic_pcm_ready() || !s_l0_bank_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    dbmap_l0_window_t win;
    const int64_t t0 = esp_timer_get_time();
    dbmap_l0_analyze_window(
        &s_l0_bank,
        job->samples,
        job->sample_count,
        job->full_scale_bits,
        job->window_ms,
        job->window_end_mono_us,
        &win
    );
    const int64_t elapsed = esp_timer_get_time() - t0;
    const uint32_t us = elapsed > 0 ? (uint32_t)elapsed : 0;

#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
    /* L1(/L2) is independent of L0 results; failure must not affect L0 publication. */
    dbmap_l1_window_t l1_win;
    memset(&l1_win, 0, sizeof(l1_win));
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
    dbmap_l2_window_t l2_win;
    memset(&l2_win, 0, sizeof(l2_win));
    uint32_t l2_combined_us = 0;
    uint32_t l2_rms_us = 0;
    uint32_t l2_incr_est_us = 0;
#else
    uint32_t l1_us = 0;
#endif
    if (s_l1_ws_ready) {
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
        const int64_t l2_t0 = esp_timer_get_time();
        dbmap_l2_analyze_window(
            &s_l1_ws,
            job->samples,
            job->sample_count,
            job->full_scale_bits,
            (uint32_t)CONFIG_DBMAP_EAR_MIC_SAMPLE_RATE_HZ,
            job->window_ms,
            job->window_end_mono_us,
            &l1_win,
            &l2_win
        );
        const int64_t l2_elapsed = esp_timer_get_time() - l2_t0;
        l2_combined_us = l2_elapsed > 0 ? (uint32_t)l2_elapsed : 0;
        {
            dbmap_l2_analyze_stages_t st;
            dbmap_l2_get_last_analyze_stages(&st);
            l2_rms_us = st.rms_us;
        }
        /* Derived estimate vs pre-L2 EAR-001 L1 baseline (not a direct L2 timer). */
        if (l2_combined_us > DBMAP_L2_L1_BASELINE_US_LAST) {
            l2_incr_est_us = l2_combined_us - DBMAP_L2_L1_BASELINE_US_LAST;
        } else {
            l2_incr_est_us = 0;
        }
#else
        const int64_t l1_t0 = esp_timer_get_time();
        dbmap_l1_analyze_window(
            &s_l1_ws,
            job->samples,
            job->sample_count,
            job->full_scale_bits,
            (uint32_t)CONFIG_DBMAP_EAR_MIC_SAMPLE_RATE_HZ,
            job->window_ms,
            job->window_end_mono_us,
            &l1_win
        );
        const int64_t l1_elapsed = esp_timer_get_time() - l1_t0;
        l1_us = l1_elapsed > 0 ? (uint32_t)l1_elapsed : 0;
#endif
    }
#endif

    portENTER_CRITICAL(&s_lock);
    s_l0_latest = win;
    s_l0_latest_ready = true;
    s_l0_analyze_count++;
    s_l0_analyze_us_last = us;
    if (us > s_l0_analyze_us_max) {
        s_l0_analyze_us_max = us;
    }
#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
    if (s_l1_ws_ready) {
        s_l1_latest = l1_win;
        s_l1_latest_ready = true;
        s_l1_analyze_count++;
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
        /* Legacy L1 analyze_us_* stay untouched while L2 reports combined time. */
        s_l2_latest = l2_win;
        s_l2_latest_ready = true;
        s_l2_analyze_count++;
        s_l2_combined_us_last = l2_combined_us;
        if (l2_combined_us > s_l2_combined_us_max) {
            s_l2_combined_us_max = l2_combined_us;
        }
        s_l2_rms_us_last = l2_rms_us;
        if (l2_rms_us > s_l2_rms_us_max) {
            s_l2_rms_us_max = l2_rms_us;
        }
        s_l2_incr_est_us_last = l2_incr_est_us;
        {
            uint32_t incr_max_est = 0;
            if (s_l2_combined_us_max > DBMAP_L2_L1_BASELINE_US_MAX) {
                incr_max_est = s_l2_combined_us_max - DBMAP_L2_L1_BASELINE_US_MAX;
            }
            s_l2_incr_est_us_max = incr_max_est;
        }
#else
        s_l1_analyze_us_last = l1_us;
        if (l1_us > s_l1_analyze_us_max) {
            s_l1_analyze_us_max = l1_us;
        }
#endif
    }
#endif
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
#endif
}

esp_err_t dbmap_ear_mic_l0_poll(void)
{
#if !CONFIG_DBMAP_EAR_MIC_L0
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!sp_mic_pcm_ready() || !s_l0_bank_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    sp_mic_pcm_job_t job;
    if (!sp_mic_take_pcm_job(&job)) {
        return ESP_OK;
    }
    return dbmap_ear_acoustic_analyze_pcm_job(&job);
#endif
}

esp_err_t dbmap_ear_mic_get_l0_window(dbmap_l0_window_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
#if !CONFIG_DBMAP_EAR_MIC_L0
    return ESP_ERR_NOT_SUPPORTED;
#else
    portENTER_CRITICAL(&s_lock);
    if (s_l0_latest_ready) {
        *out = s_l0_latest;
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
#endif
}

esp_err_t dbmap_ear_mic_get_l0_perf(dbmap_ear_mic_l0_perf_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
#if !CONFIG_DBMAP_EAR_MIC_L0
    out->enabled = false;
    return ESP_OK;
#else
    out->enabled = true;
    out->pcm_ready = sp_mic_pcm_ready();
    out->pcm_cap_samples = sp_mic_pcm_cap_samples();
    out->pcm_bytes_total = sp_mic_pcm_bytes_total();
    out->pending_overwrite = sp_mic_pcm_pending_overwrite();
    out->free_heap_after_init = sp_mic_pcm_free_heap_after_init();
    out->free_spiram_after_init = sp_mic_pcm_free_spiram_after_init();
    portENTER_CRITICAL(&s_lock);
    out->analyze_count = s_l0_analyze_count;
    out->analyze_us_last = s_l0_analyze_us_last;
    out->analyze_us_max = s_l0_analyze_us_max;
    portEXIT_CRITICAL(&s_lock);
    {
        dbmap_l0_analyze_stages_t st;
        dbmap_l0_get_last_analyze_stages(&st);
        out->stage_reset_us = st.reset_us;
        out->stage_sample_loop_us = st.sample_loop_us;
        out->stage_post_us = st.post_us;
    }
    return ESP_OK;
#endif
}

esp_err_t dbmap_ear_mic_get_l1_window(dbmap_l1_window_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
#if !defined(CONFIG_DBMAP_EAR_OBS_L1) || !CONFIG_DBMAP_EAR_OBS_L1 || !CONFIG_DBMAP_EAR_MIC_L0
    return ESP_ERR_NOT_SUPPORTED;
#else
    portENTER_CRITICAL(&s_lock);
    if (s_l1_latest_ready) {
        *out = s_l1_latest;
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
#endif
}

esp_err_t dbmap_ear_mic_get_l1_perf(dbmap_ear_mic_l1_perf_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
#if !defined(CONFIG_DBMAP_EAR_OBS_L1) || !CONFIG_DBMAP_EAR_OBS_L1 || !CONFIG_DBMAP_EAR_MIC_L0
    out->enabled = false;
    return ESP_OK;
#else
    out->enabled = true;
    portENTER_CRITICAL(&s_lock);
    out->analyze_count = s_l1_analyze_count;
    out->analyze_us_last = s_l1_analyze_us_last;
    out->analyze_us_max = s_l1_analyze_us_max;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
#endif
}

esp_err_t dbmap_ear_mic_get_l2_window(dbmap_l2_window_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
#if !defined(CONFIG_DBMAP_EAR_OBS_L2) || !CONFIG_DBMAP_EAR_OBS_L2 || \
    !defined(CONFIG_DBMAP_EAR_OBS_L1) || !CONFIG_DBMAP_EAR_OBS_L1 || !CONFIG_DBMAP_EAR_MIC_L0
    return ESP_ERR_NOT_SUPPORTED;
#else
    portENTER_CRITICAL(&s_lock);
    if (s_l2_latest_ready) {
        *out = s_l2_latest;
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
#endif
}

esp_err_t dbmap_ear_mic_get_l2_perf(dbmap_ear_mic_l2_perf_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
#if !defined(CONFIG_DBMAP_EAR_OBS_L2) || !CONFIG_DBMAP_EAR_OBS_L2 || \
    !defined(CONFIG_DBMAP_EAR_OBS_L1) || !CONFIG_DBMAP_EAR_OBS_L1 || !CONFIG_DBMAP_EAR_MIC_L0
    out->enabled = false;
    return ESP_OK;
#else
    out->enabled = true;
    out->baseline_l1_us_last = DBMAP_L2_L1_BASELINE_US_LAST;
    out->baseline_l1_us_max = DBMAP_L2_L1_BASELINE_US_MAX;
    portENTER_CRITICAL(&s_lock);
    out->analyze_count = s_l2_analyze_count;
    out->combined_us_last = s_l2_combined_us_last;
    out->combined_us_max = s_l2_combined_us_max;
    out->incremental_estimated_us_last = s_l2_incr_est_us_last;
    out->incremental_estimated_us_max = s_l2_incr_est_us_max;
    out->rms_us_last = s_l2_rms_us_last;
    out->rms_us_max = s_l2_rms_us_max;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
#endif
}
