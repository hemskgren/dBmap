#include "sdkconfig.h"

#include "dbmap_ear_acoustic.h"

#include <string.h>

#include "sp_audio_analyze.h"
#include "sp_mic_pcm.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "ear_acoustic";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

#if CONFIG_SP_AUDIO_L0
static sp_audio_analyze_ctx_t s_analyze_ctx;
static sp_audio_window_result_t s_latest;
static bool s_latest_ready;
static uint8_t s_active_mic_count;
static uint32_t s_l0_analyze_count;
static uint32_t s_l0_analyze_us_last;
static uint32_t s_l0_analyze_us_max;
#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
static uint32_t s_l1_analyze_count;
static uint32_t s_l1_analyze_us_last;
static uint32_t s_l1_analyze_us_max;
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
static uint32_t s_l2_analyze_count;
static uint32_t s_l2_combined_us_last;
static uint32_t s_l2_combined_us_max;
static uint32_t s_l2_incr_est_us_last;
static uint32_t s_l2_incr_est_us_max;
static uint32_t s_l2_rms_us_last;
static uint32_t s_l2_rms_us_max;
#endif
#endif

static void pcm_ready_hook(uint32_t sample_rate_hz)
{
    sp_audio_analyze_ctx_init(&s_analyze_ctx, sample_rate_hz);
    memset(&s_latest, 0, sizeof(s_latest));
    s_latest_ready = false;
    s_active_mic_count = 0;
    s_l0_analyze_count = 0;
    s_l0_analyze_us_last = 0;
    s_l0_analyze_us_max = 0;
    ESP_LOGI(
        TAG,
        "multi-mic analyze ready sample_rate=%lu (serial L1 workspace reuse)",
        (unsigned long)sample_rate_hz
    );
#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
    s_l1_analyze_count = 0;
    s_l1_analyze_us_last = 0;
    s_l1_analyze_us_max = 0;
    ESP_LOGI(
        TAG,
        "L1 enabled fft=%u (one workspace ≈22.5 KiB, reused per channel)",
        (unsigned)SP_ACOUSTIC_L1_FFT_SIZE
    );
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
    s_l2_analyze_count = 0;
    s_l2_combined_us_last = 0;
    s_l2_combined_us_max = 0;
    s_l2_incr_est_us_last = 0;
    s_l2_incr_est_us_max = 0;
    s_l2_rms_us_last = 0;
    s_l2_rms_us_max = 0;
    ESP_LOGI(
        TAG,
        "L2 enabled compute=1 publish=%d (fused L1+L2 per active mic)",
#if defined(CONFIG_DBMAP_EAR_OBS_L2_PUBLISH) && CONFIG_DBMAP_EAR_OBS_L2_PUBLISH
        1
#else
        0
#endif
    );
#endif
#endif
}

static void fill_analyze_input(const sp_mic_pcm_job_t *job, sp_audio_analyze_input_t *in)
{
    memset(in, 0, sizeof(*in));
    in->full_scale_bits = job->full_scale_bits;
    in->window_ms = job->window_ms;
    in->window_end_mono_us = job->window_end_mono_us;
    in->window_seq = job->window_seq;
    in->pcm_channel_mask = job->pcm_channel_mask;
    for (uint8_t ch = 0; ch < SP_AUDIO_ANALYZE_MAX_CHANNELS; ++ch) {
        in->samples_ch[ch] = job->samples_ch[ch];
        in->sample_count_ch[ch] = job->sample_count_ch[ch];
    }
}

static uint8_t resolve_active_count(const sp_mic_pcm_job_t *job)
{
    uint8_t n = job->active_mic_count;
    if (n < 1u || n > SP_AUDIO_ANALYZE_MAX_CHANNELS) {
        n = sp_mic_pcm_retained_channels();
    }
    if (n < 1u || n > SP_AUDIO_ANALYZE_MAX_CHANNELS) {
        n = 1u;
    }
    return n;
}
#endif

esp_err_t dbmap_ear_acoustic_register(void)
{
#if !CONFIG_SP_AUDIO_L0
    return ESP_OK;
#else
    sp_mic_set_pcm_ready_hook(pcm_ready_hook);
    return ESP_OK;
#endif
}

esp_err_t dbmap_ear_acoustic_analyze_pcm_job(const sp_mic_pcm_job_t *job)
{
#if !CONFIG_SP_AUDIO_L0
    (void)job;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (job == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sp_mic_pcm_ready() || !s_analyze_ctx.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t active = resolve_active_count(job);
    sp_audio_analyze_input_t in;
    fill_analyze_input(job, &in);

    /* Mic0 primary path still requires samples when only ch0 is expected. */
    if (active == 1u && (job->samples == NULL || job->sample_count == 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    sp_audio_window_result_t result;
    const int64_t t0 = esp_timer_get_time();
    const int rc = sp_audio_analyze_channels(&s_analyze_ctx, &in, active, &result);
    const int64_t elapsed = esp_timer_get_time() - t0;
    const uint32_t us = elapsed > 0 ? (uint32_t)elapsed : 0;
    if (rc != SP_AUDIO_ANALYZE_OK) {
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&s_lock);
    s_latest = result;
    s_latest_ready = true;
    s_active_mic_count = active;
    s_l0_analyze_count++;
    s_l0_analyze_us_last = us;
    if (us > s_l0_analyze_us_max) {
        s_l0_analyze_us_max = us;
    }
#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
    /* Fused L1+L2: count L1 windows (us stays 0; wall time is L2 combined). */
    s_l1_analyze_count++;
    s_l2_analyze_count++;
    s_l2_combined_us_last = us;
    if (us > s_l2_combined_us_max) {
        s_l2_combined_us_max = us;
    }
    {
        sp_acoustic_l2_analyze_stages_t st;
        sp_acoustic_l2_get_last_analyze_stages(&st);
        s_l2_rms_us_last = st.rms_us;
        if (st.rms_us > s_l2_rms_us_max) {
            s_l2_rms_us_max = st.rms_us;
        }
    }
    if (us > DBMAP_L2_L1_BASELINE_US_LAST) {
        s_l2_incr_est_us_last = us - DBMAP_L2_L1_BASELINE_US_LAST;
    } else {
        s_l2_incr_est_us_last = 0;
    }
    {
        uint32_t incr_max_est = 0;
        if (s_l2_combined_us_max > DBMAP_L2_L1_BASELINE_US_MAX) {
            incr_max_est = s_l2_combined_us_max - DBMAP_L2_L1_BASELINE_US_MAX;
        }
        s_l2_incr_est_us_max = incr_max_est;
    }
#else
    s_l1_analyze_count++;
    s_l1_analyze_us_last = us;
    if (us > s_l1_analyze_us_max) {
        s_l1_analyze_us_max = us;
    }
#endif
#endif
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
#endif
}

esp_err_t dbmap_ear_mic_l0_poll(void)
{
#if !CONFIG_SP_AUDIO_L0
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!sp_mic_pcm_ready() || !s_analyze_ctx.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    sp_mic_pcm_job_t job;
    if (!sp_mic_take_pcm_job(&job)) {
        return ESP_OK;
    }
    return dbmap_ear_acoustic_analyze_pcm_job(&job);
#endif
}

esp_err_t dbmap_ear_mic_get_l0_window(sp_acoustic_l0_window_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
#if !CONFIG_SP_AUDIO_L0
    return ESP_ERR_NOT_SUPPORTED;
#else
    /* Mic0 alias: ESP_OK with valid=false until first analysis (legacy contract). */
    portENTER_CRITICAL(&s_lock);
    if (s_latest_ready && (s_latest.result_mask & 0x01u) != 0u) {
        *out = s_latest.channels[0].l0;
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
#endif
}

esp_err_t dbmap_ear_mic_get_l0_window_channel(uint8_t channel_id, sp_acoustic_l0_window_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
#if !CONFIG_SP_AUDIO_L0
    (void)channel_id;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (channel_id == 0u) {
        return dbmap_ear_mic_get_l0_window(out);
    }
    portENTER_CRITICAL(&s_lock);
    if (!s_latest_ready || channel_id >= s_active_mic_count
        || (s_latest.result_mask & (uint8_t)(1u << channel_id)) == 0u) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_latest.channels[channel_id].l0;
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
#if !CONFIG_SP_AUDIO_L0
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
        sp_acoustic_l0_analyze_stages_t st;
        sp_acoustic_l0_get_last_analyze_stages(&st);
        out->stage_reset_us = st.reset_us;
        out->stage_sample_loop_us = st.sample_loop_us;
        out->stage_post_us = st.post_us;
    }
    return ESP_OK;
#endif
}

esp_err_t dbmap_ear_mic_get_l1_window(sp_acoustic_l1_window_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
#if !defined(CONFIG_DBMAP_EAR_OBS_L1) || !CONFIG_DBMAP_EAR_OBS_L1 || !CONFIG_SP_AUDIO_L0
    return ESP_ERR_NOT_SUPPORTED;
#else
    portENTER_CRITICAL(&s_lock);
    if (s_latest_ready && (s_latest.result_mask & 0x01u) != 0u) {
        *out = s_latest.channels[0].l1;
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
#endif
}

esp_err_t dbmap_ear_mic_get_l1_window_channel(uint8_t channel_id, sp_acoustic_l1_window_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
#if !defined(CONFIG_DBMAP_EAR_OBS_L1) || !CONFIG_DBMAP_EAR_OBS_L1 || !CONFIG_SP_AUDIO_L0
    (void)channel_id;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (channel_id == 0u) {
        return dbmap_ear_mic_get_l1_window(out);
    }
    portENTER_CRITICAL(&s_lock);
    if (!s_latest_ready || channel_id >= s_active_mic_count
        || (s_latest.result_mask & (uint8_t)(1u << channel_id)) == 0u) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_latest.channels[channel_id].l1;
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
#if !defined(CONFIG_DBMAP_EAR_OBS_L1) || !CONFIG_DBMAP_EAR_OBS_L1 || !CONFIG_SP_AUDIO_L0
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

esp_err_t dbmap_ear_mic_get_l2_window(sp_acoustic_l2_window_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
#if !defined(CONFIG_DBMAP_EAR_OBS_L2) || !CONFIG_DBMAP_EAR_OBS_L2 || \
    !defined(CONFIG_DBMAP_EAR_OBS_L1) || !CONFIG_DBMAP_EAR_OBS_L1 || !CONFIG_SP_AUDIO_L0
    return ESP_ERR_NOT_SUPPORTED;
#else
    portENTER_CRITICAL(&s_lock);
    if (s_latest_ready && (s_latest.result_mask & 0x01u) != 0u) {
        *out = s_latest.channels[0].l2;
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
#endif
}

esp_err_t dbmap_ear_mic_get_l2_window_channel(uint8_t channel_id, sp_acoustic_l2_window_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
#if !defined(CONFIG_DBMAP_EAR_OBS_L2) || !CONFIG_DBMAP_EAR_OBS_L2 || \
    !defined(CONFIG_DBMAP_EAR_OBS_L1) || !CONFIG_DBMAP_EAR_OBS_L1 || !CONFIG_SP_AUDIO_L0
    (void)channel_id;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (channel_id == 0u) {
        return dbmap_ear_mic_get_l2_window(out);
    }
    portENTER_CRITICAL(&s_lock);
    if (!s_latest_ready || channel_id >= s_active_mic_count
        || (s_latest.result_mask & (uint8_t)(1u << channel_id)) == 0u) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_latest.channels[channel_id].l2;
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
    !defined(CONFIG_DBMAP_EAR_OBS_L1) || !CONFIG_DBMAP_EAR_OBS_L1 || !CONFIG_SP_AUDIO_L0
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

esp_err_t dbmap_ear_acoustic_get_window_result(sp_audio_window_result_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
#if !CONFIG_SP_AUDIO_L0
    return ESP_ERR_NOT_SUPPORTED;
#else
    portENTER_CRITICAL(&s_lock);
    if (s_latest_ready) {
        *out = s_latest;
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
#endif
}

uint8_t dbmap_ear_acoustic_active_mic_count(void)
{
#if !CONFIG_SP_AUDIO_L0
    return 0;
#else
    uint8_t n;
    portENTER_CRITICAL(&s_lock);
    n = s_active_mic_count;
    portEXIT_CRITICAL(&s_lock);
    return n;
#endif
}
