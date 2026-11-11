#include "dbmap_ear_mic.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "dbmap_acoustic_l0.h"
#include "dbmap_acoustic_l2.h"
#include "dbmap_audio_metrics.h"
#include "dbmap_background_rms.h"
#include "dbmap_ear_mic_capture_diag.h"
#include "dbmap_ear_mic_channel_api.h"
#include "dbmap_health.h"
#include "dbmap_i2s_stereo.h"
#include "dbmap_mic_count.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic_model.h"
#include "sdkconfig.h"

static const char *TAG = "ear_mic";

#define EAR_MIC_READ_FRAMES 256
#define EAR_MIC_TASK_STACK 6144
/* Max slot words per read: stereo needs 2 words per sample period. */
#define EAR_MIC_SLOT_BUF_WORDS (EAR_MIC_READ_FRAMES * (int)DBMAP_MIC_COUNT_MAX)

typedef struct {
    float rms_dbfs;
    float peak_dbfs;
    uint32_t sample_count;
    uint32_t window_ms;
    /* Separate read-result counters — never merge these in telemetry. */
    uint32_t read_errors;   /* i2s_channel_read returned a hard error */
    uint32_t timeout_reads; /* ESP_ERR_TIMEOUT (queue wait expired) */
    uint32_t empty_reads;   /* ESP_OK with no whole slot frames */
    uint32_t missing_data;
    uint32_t clipped_samples;
    uint8_t counters_capped; /* 1 if any per-window counter hit the display cap */
    /* Temporary mic-presence investigation (no raw PCM published). */
    int32_t raw_min;
    int32_t raw_max;
    uint32_t zero_sample_count;
    uint32_t full_scale_sample_count;
    uint32_t unique_sample_count;
    uint32_t consecutive_identical_samples;
    uint8_t unique_capped;
    /* Per-window startup / capture diagnostics (no raw PCM). */
    uint32_t window_seq;           /* 1-based since ear_mic task start */
    uint32_t ms_since_i2s_enable;  /* wall time from i2s_channel_enable */
    uint32_t read_calls;
    uint32_t bytes_received;
    uint32_t complete_frames;      /* whole 32-bit slots included in RMS/peak */
    uint32_t partial_reads;        /* 0 < bytes_read < request size */
    /* Pre-extract 32-bit I2S slot extremes (not PCM). */
    uint32_t raw_slot_min;
    uint32_t raw_slot_max;
    uint32_t slot_zero_count;      /* slot word == 0 before >> 8 */
    uint32_t slot_msb_set_count;   /* bit31 set before >> 8 (→ negative after extract) */
    /* Presence ≠ health: samples alone do not imply a healthy mic. */
    char channel_presence[16]; /* no_samples|silent|clipping|active */
    uint8_t all_zero_window;       /* 1 if sample_count>0 and every sample == 0 */
    char transport_status[16]; /* I2S fill health: ok|degraded|error */
    char microphone_status[24]; /* unavailable|disconnected_suspected|noise_suspected|signal_present|unknown */
    char classification_reason[48];
    int64_t measured_at_us;
} ear_audio_snapshot_t;

/* Bounded unique-value table for window diagnostics (not a full histogram). */
#define EAR_UNIQUE_CAP 1024u
static int32_t s_unique_vals[DBMAP_MIC_COUNT_MAX][EAR_UNIQUE_CAP];
static uint8_t s_unique_occ[DBMAP_MIC_COUNT_MAX][EAR_UNIQUE_CAP];

static void unique_reset(uint8_t ch)
{
    if (ch >= DBMAP_MIC_COUNT_MAX) {
        return;
    }
    memset(s_unique_occ[ch], 0, sizeof(s_unique_occ[ch]));
}

static void unique_add(
    uint8_t ch,
    int32_t sample,
    uint32_t *unique_count,
    uint8_t *unique_capped
)
{
    if (ch >= DBMAP_MIC_COUNT_MAX || unique_count == NULL || unique_capped == NULL) {
        return;
    }
    if (*unique_count >= EAR_UNIQUE_CAP) {
        *unique_capped = 1;
        return;
    }
    uint32_t hash = (uint32_t)sample * 2654435761u;
    for (uint32_t probe = 0; probe < EAR_UNIQUE_CAP; ++probe) {
        uint32_t idx = (hash + probe) % EAR_UNIQUE_CAP;
        if (s_unique_occ[ch][idx] == 0) {
            s_unique_occ[ch][idx] = 1;
            s_unique_vals[ch][idx] = sample;
            (*unique_count)++;
            return;
        }
        if (s_unique_vals[ch][idx] == sample) {
            return;
        }
    }
    *unique_capped = 1;
}

/* Cap telemetry counters so a tight empty-read path cannot push JSON into huge ints.
 * Cap hit is signaled via counters_capped; status still uses sample_count / missing_data. */
#define EAR_MIC_COUNTER_CAP 10000u

static void bump_counter(uint32_t *counter, uint8_t *capped)
{
    if (*counter < EAR_MIC_COUNTER_CAP) {
        (*counter)++;
    } else if (capped != NULL) {
        *capped = 1;
    }
}

static const dbmap_effective_t *s_cfg;
static dbmap_ear_mic_config_t s_mic_cfg;
static const dbmap_ear_mic_model_desc_t *s_model;
static i2s_chan_handle_t s_rx;
static bool s_rx_ready;
/* Index 0 is primary (Observation / L0 / activity). Index 1 when live_mic_count==2. */
static ear_audio_snapshot_t s_latest[DBMAP_MIC_COUNT_MAX];
static uint8_t s_live_mic_count = DBMAP_MIC_COUNT_DEFAULT;
static char s_status_cache[16] = "unavailable";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
/* Keep I2S read scratch off the task stack (was overflowing a 4 KiB stack → TG1WDT). */
static uint32_t s_slot_buf[EAR_MIC_SLOT_BUF_WORDS];
static int64_t s_i2s_enabled_us;
static uint32_t s_window_seq;
/* Values actually passed to i2s_new_channel (not guesses). */
static uint32_t s_dma_desc_num;
static uint32_t s_dma_frame_num;
static uint32_t s_slot_words_per_frame = 1;
static uint32_t s_request_bytes;
static uint32_t s_incomplete_stereo_frames_window;
static uint32_t s_measure_window_us_last;
static char s_slot_mode_name[16] = "mono_left";
#if CONFIG_DBMAP_EAR_MIC_CAPTURE_DIAG
static int64_t s_capture_diag_last_us;
#endif

#if CONFIG_DBMAP_EAR_MIC_BACKGROUND
/* Capacity covers configured window at configured analysis period (+margin). */
#define EAR_BG_RMS_CAP                                                                 \
    (((uint32_t)CONFIG_DBMAP_EAR_MIC_BACKGROUND_WINDOW_MS                               \
      / (uint32_t)CONFIG_DBMAP_EAR_MIC_WINDOW_MS)                                       \
     + 8u)
static dbmap_bg_rms_sample_t s_bg_entries[EAR_BG_RMS_CAP];
static dbmap_bg_rms_hist_t s_bg_hist;
#endif

#if CONFIG_DBMAP_EAR_MIC_L0
/* Double-buffered SPIRAM PCM so L0 analyze never stalls the I2S read loop. */
static int32_t *s_l0_pcm[2];
static uint32_t s_l0_pcm_cap;
static int s_l0_fill_idx;
static dbmap_l0_filter_bank_t s_l0_bank;
static dbmap_l0_window_t s_l0_latest;
static bool s_l0_latest_ready;
static bool s_l0_pcm_ready;

typedef struct {
    bool pending;
    int buf_idx;
    size_t sample_count;
    uint32_t window_ms;
    int64_t window_end_mono_us;
    int full_scale_bits;
} ear_l0_pending_t;

static ear_l0_pending_t s_l0_pending;
static uint32_t s_l0_analyze_count;
static uint32_t s_l0_analyze_us_last;
static uint32_t s_l0_analyze_us_max;
static uint32_t s_l0_pending_overwrite;
static uint32_t s_l0_free_heap_after_init;
static uint32_t s_l0_free_spiram_after_init;
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

static int gpio_or_unused(int gpio)
{
    return gpio < 0 ? I2S_GPIO_UNUSED : gpio;
}

const char *dbmap_ear_mic_model_id(void)
{
    if (s_model != NULL) {
        return s_model->id;
    }
    const dbmap_ear_mic_model_desc_t *selected = dbmap_ear_mic_model_selected();
    return selected != NULL ? selected->id : "unknown";
}

const char *dbmap_ear_mic_subsystem_status(void)
{
    portENTER_CRITICAL(&s_lock);
    strncpy(
        s_status_cache,
        s_latest[0].transport_status[0] ? s_latest[0].transport_status : "unavailable",
        sizeof(s_status_cache) - 1
    );
    s_status_cache[sizeof(s_status_cache) - 1] = '\0';
    portEXIT_CRITICAL(&s_lock);
    return s_status_cache;
}

static void snapshot_to_view(const ear_audio_snapshot_t *snap, dbmap_ear_mic_window_view_t *view)
{
    memset(view, 0, sizeof(*view));
    if (snap == NULL) {
        return;
    }
    view->rms_dbfs = snap->rms_dbfs;
    view->peak_dbfs = snap->peak_dbfs;
    view->sample_count = snap->sample_count;
    view->window_ms = snap->window_ms;
    view->window_seq = snap->window_seq;
    view->ms_since_i2s_enable = snap->ms_since_i2s_enable;
    view->measured_at_us = snap->measured_at_us;
    view->zero_sample_count = snap->zero_sample_count;
    view->all_zero_window = snap->all_zero_window;
    view->clipped_samples = snap->clipped_samples;
    view->read_errors = snap->read_errors;
    view->timeout_reads = snap->timeout_reads;
    view->empty_reads = snap->empty_reads;
    view->missing_data = snap->missing_data;
    view->counters_capped = snap->counters_capped;
    view->raw_min = snap->raw_min;
    view->raw_max = snap->raw_max;
    view->full_scale_sample_count = snap->full_scale_sample_count;
    view->unique_sample_count = snap->unique_sample_count;
    view->consecutive_identical_samples = snap->consecutive_identical_samples;
    view->unique_capped = snap->unique_capped;
    view->read_calls = snap->read_calls;
    view->bytes_received = snap->bytes_received;
    view->complete_frames = snap->complete_frames;
    view->partial_reads = snap->partial_reads;
    view->raw_slot_min = snap->raw_slot_min;
    view->raw_slot_max = snap->raw_slot_max;
    view->slot_zero_count = snap->slot_zero_count;
    view->slot_msb_set_count = snap->slot_msb_set_count;
    strncpy(view->channel_presence, snap->channel_presence, sizeof(view->channel_presence) - 1);
    strncpy(view->transport_status, snap->transport_status, sizeof(view->transport_status) - 1);
    strncpy(view->microphone_status, snap->microphone_status, sizeof(view->microphone_status) - 1);
    strncpy(
        view->classification_reason,
        snap->classification_reason,
        sizeof(view->classification_reason) - 1
    );
}

esp_err_t dbmap_ear_mic_get_levels(dbmap_ear_mic_levels_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    dbmap_ear_mic_window_view_t view;
    portENTER_CRITICAL(&s_lock);
    /* Primary path remains mic 0 (left) for Observation / activity compatibility. */
    snapshot_to_view(&s_latest[0], &view);
    portEXIT_CRITICAL(&s_lock);

    dbmap_ear_mic_primary_levels_t primary;
    if (dbmap_ear_mic_fill_primary_levels(&view, &primary) != DBMAP_EAR_MIC_API_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->valid = primary.valid;
    out->rms_dbfs = primary.rms_dbfs;
    out->peak_dbfs = primary.peak_dbfs;
    out->window_ms = primary.window_ms;
    out->measured_at_us = primary.measured_at_us;
    out->window_seq = primary.window_seq;
    out->ms_since_i2s_enable = primary.ms_since_i2s_enable;
    strncpy(out->transport_status, primary.transport_status, sizeof(out->transport_status) - 1);
    strncpy(out->microphone_status, primary.microphone_status, sizeof(out->microphone_status) - 1);
    return ESP_OK;
}

esp_err_t dbmap_ear_mic_get_channel(uint8_t index, dbmap_ear_mic_channel_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    dbmap_ear_mic_window_view_t view;
    uint8_t live;
    uint32_t rate_hz;
    portENTER_CRITICAL(&s_lock);
    live = s_live_mic_count;
    rate_hz = (uint32_t)s_mic_cfg.sample_rate_hz;
    if (live < 1u || live > DBMAP_MIC_COUNT_MAX) {
        live = 1u;
    }
    if (index >= live) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    snapshot_to_view(&s_latest[index], &view);
    portEXIT_CRITICAL(&s_lock);

    if (dbmap_ear_mic_fill_channel(index, live, &view, rate_hz, out) != DBMAP_EAR_MIC_API_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t dbmap_ear_mic_get_stream(dbmap_ear_mic_stream_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    dbmap_ear_mic_window_view_t views[DBMAP_MIC_COUNT_MAX];
    uint8_t live;
    uint8_t configured;
    uint32_t incomplete;
    char slot_mode[sizeof(s_slot_mode_name)];
    portENTER_CRITICAL(&s_lock);
    live = s_live_mic_count;
    if (live < 1u || live > DBMAP_MIC_COUNT_MAX) {
        live = 1u;
    }
    configured = (s_cfg != NULL) ? s_cfg->microphone_count : live;
    incomplete = s_incomplete_stereo_frames_window;
    strncpy(slot_mode, s_slot_mode_name, sizeof(slot_mode) - 1);
    slot_mode[sizeof(slot_mode) - 1] = '\0';
    for (uint8_t i = 0; i < live; ++i) {
        snapshot_to_view(&s_latest[i], &views[i]);
    }
    portEXIT_CRITICAL(&s_lock);

    if (!dbmap_mic_count_supported(configured)) {
        configured = DBMAP_MIC_COUNT_DEFAULT;
    }
    if (dbmap_ear_mic_fill_stream(
            live,
            configured,
            views,
            slot_mode,
            incomplete,
            out
        )
        != DBMAP_EAR_MIC_API_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t dbmap_ear_mic_l0_poll(void)
{
#if !CONFIG_DBMAP_EAR_MIC_L0
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_l0_pcm_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    ear_l0_pending_t job;
    portENTER_CRITICAL(&s_lock);
    job = s_l0_pending;
    if (!job.pending) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    s_l0_pending.pending = false;
    portEXIT_CRITICAL(&s_lock);

    if (job.buf_idx < 0 || job.buf_idx > 1 || s_l0_pcm[job.buf_idx] == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    dbmap_l0_window_t win;
    const int64_t t0 = esp_timer_get_time();
    dbmap_l0_analyze_window(
        &s_l0_bank,
        s_l0_pcm[job.buf_idx],
        job.sample_count,
        job.full_scale_bits,
        job.window_ms,
        job.window_end_mono_us,
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
            s_l0_pcm[job.buf_idx],
            job.sample_count,
            job.full_scale_bits,
            (uint32_t)CONFIG_DBMAP_EAR_MIC_SAMPLE_RATE_HZ,
            job.window_ms,
            job.window_end_mono_us,
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
            s_l0_pcm[job.buf_idx],
            job.sample_count,
            job.full_scale_bits,
            (uint32_t)CONFIG_DBMAP_EAR_MIC_SAMPLE_RATE_HZ,
            job.window_ms,
            job.window_end_mono_us,
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
    portENTER_CRITICAL(&s_lock);
    out->pcm_ready = s_l0_pcm_ready;
    out->pcm_cap_samples = s_l0_pcm_cap;
    out->pcm_bytes_total = s_l0_pcm_ready ? (s_l0_pcm_cap * sizeof(int32_t) * 2u) : 0;
    out->analyze_count = s_l0_analyze_count;
    out->analyze_us_last = s_l0_analyze_us_last;
    out->analyze_us_max = s_l0_analyze_us_max;
    out->pending_overwrite = s_l0_pending_overwrite;
    out->free_heap_after_init = s_l0_free_heap_after_init;
    out->free_spiram_after_init = s_l0_free_spiram_after_init;
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

static void set_status(ear_audio_snapshot_t *snap, uint32_t expected_samples)
{
    /* Transport uses a combined non-data hint; telemetry keeps counters separate. */
    const uint32_t non_data_reads = snap->timeout_reads + snap->empty_reads;
    const char *transport = dbmap_audio_window_status(
        snap->sample_count,
        expected_samples,
        snap->read_errors,
        non_data_reads,
        snap->missing_data
    );
    strncpy(snap->transport_status, transport, sizeof(snap->transport_status) - 1);
    snap->transport_status[sizeof(snap->transport_status) - 1] = '\0';

    dbmap_audio_mic_class_input_t mic_in = {
        .transport_status = snap->transport_status,
        .sample_count = snap->sample_count,
        .rms_dbfs = snap->rms_dbfs,
        .peak_dbfs = snap->peak_dbfs,
        .diag = {
            .raw_min = snap->raw_min,
            .raw_max = snap->raw_max,
            .zero_sample_count = snap->zero_sample_count,
            .full_scale_sample_count = snap->full_scale_sample_count,
            .unique_sample_count = snap->unique_sample_count,
            .consecutive_identical_samples = snap->consecutive_identical_samples,
            .unique_capped = snap->unique_capped,
        },
    };
    const char *mic = dbmap_audio_classify_microphone(
        &mic_in,
        snap->classification_reason,
        sizeof(snap->classification_reason)
    );
    strncpy(snap->microphone_status, mic, sizeof(snap->microphone_status) - 1);
    snap->microphone_status[sizeof(snap->microphone_status) - 1] = '\0';
}

/**
 * Attach Ear fields onto hello/health/keepalive reported state (same path as wifi_rssi_dbm).
 * When live_mic_count==2, reports indexes 0 (left) and 1 (right). Observation path
 * still consumes mic 0 only — no Observation schema change.
 */
static void enrich_mic_object(cJSON *mic, const ear_audio_snapshot_t *snap, uint8_t index)
{
    if (mic == NULL || snap == NULL || s_model == NULL) {
        return;
    }
    const char *transport =
        snap->transport_status[0] ? snap->transport_status : "unavailable";
    const char *mic_status =
        snap->microphone_status[0] ? snap->microphone_status : "unavailable";
    const char *presence =
        snap->channel_presence[0] ? snap->channel_presence : "no_samples";

    cJSON_AddNumberToObject(mic, "index", (double)index);
    cJSON_AddStringToObject(mic, "model", s_model->id);
    cJSON_AddNumberToObject(mic, "channels", 1);
    cJSON_AddStringToObject(mic, "slot", index == 0 ? "left" : "right");
    cJSON_AddNumberToObject(mic, "rms_dbfs", snap->rms_dbfs);
    cJSON_AddNumberToObject(mic, "peak_dbfs", snap->peak_dbfs);
    cJSON_AddNumberToObject(mic, "sample_count", snap->sample_count);
    cJSON_AddNumberToObject(
        mic,
        "expected_sample_count",
        dbmap_audio_expected_samples((uint32_t)s_mic_cfg.sample_rate_hz, snap->window_ms)
    );
    cJSON_AddNumberToObject(mic, "window_ms", snap->window_ms);
    /* Shared I2S read counters (same DMA stream for both slots). */
    cJSON_AddNumberToObject(mic, "read_errors", snap->read_errors);
    cJSON_AddNumberToObject(mic, "timeout_reads", snap->timeout_reads);
    cJSON_AddNumberToObject(mic, "empty_reads", snap->empty_reads);
    cJSON_AddNumberToObject(mic, "missing_data", snap->missing_data);
    cJSON_AddNumberToObject(mic, "clipped_samples", snap->clipped_samples);
    cJSON_AddBoolToObject(mic, "counters_capped", snap->counters_capped != 0);
    cJSON_AddBoolToObject(mic, "all_zero_window", snap->all_zero_window != 0);
    cJSON_AddStringToObject(mic, "channel_presence", presence);
    cJSON_AddNumberToObject(mic, "raw_min", snap->raw_min);
    cJSON_AddNumberToObject(mic, "raw_max", snap->raw_max);
    cJSON_AddNumberToObject(mic, "zero_sample_count", snap->zero_sample_count);
    cJSON_AddNumberToObject(mic, "full_scale_sample_count", snap->full_scale_sample_count);
    cJSON_AddNumberToObject(mic, "unique_sample_count", snap->unique_sample_count);
    cJSON_AddNumberToObject(mic, "consecutive_identical_samples", snap->consecutive_identical_samples);
    cJSON_AddBoolToObject(mic, "unique_capped", snap->unique_capped != 0);
    cJSON_AddNumberToObject(mic, "window_seq", snap->window_seq);
    cJSON_AddNumberToObject(mic, "ms_since_i2s_enable", snap->ms_since_i2s_enable);
    cJSON_AddNumberToObject(mic, "read_calls", snap->read_calls);
    cJSON_AddNumberToObject(mic, "bytes_received", snap->bytes_received);
    cJSON_AddNumberToObject(mic, "complete_frames", snap->complete_frames);
    cJSON_AddNumberToObject(mic, "partial_reads", snap->partial_reads);
    cJSON_AddNumberToObject(mic, "raw_slot_min", (double)snap->raw_slot_min);
    cJSON_AddNumberToObject(mic, "raw_slot_max", (double)snap->raw_slot_max);
    {
        char hex[11];
        snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)snap->raw_slot_min);
        cJSON_AddStringToObject(mic, "raw_slot_min_hex", hex);
        snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)snap->raw_slot_max);
        cJSON_AddStringToObject(mic, "raw_slot_max_hex", hex);
    }
    cJSON_AddNumberToObject(mic, "slot_zero_count", snap->slot_zero_count);
    cJSON_AddNumberToObject(mic, "slot_msb_set_count", snap->slot_msb_set_count);
    cJSON_AddStringToObject(mic, "status", transport);
    cJSON_AddStringToObject(mic, "microphone_status", mic_status);
    if (snap->classification_reason[0] != '\0') {
        cJSON_AddStringToObject(mic, "classification_reason", snap->classification_reason);
    }
    if (snap->measured_at_us > 0) {
        cJSON_AddNumberToObject(
            mic,
            "measured_at_uptime_s",
            (double)(snap->measured_at_us / 1000000LL)
        );
    }
}

static void enrich_health(cJSON *state, const dbmap_effective_t *cfg, bool include_diag)
{
    (void)include_diag;
    if (state == NULL || cfg == NULL || s_model == NULL) {
        return;
    }

    ear_audio_snapshot_t snaps[DBMAP_MIC_COUNT_MAX];
    uint8_t live_count;
    uint32_t incomplete_stereo;
    portENTER_CRITICAL(&s_lock);
    live_count = s_live_mic_count;
    incomplete_stereo = s_incomplete_stereo_frames_window;
    for (uint8_t i = 0; i < DBMAP_MIC_COUNT_MAX; ++i) {
        snaps[i] = s_latest[i];
    }
    portEXIT_CRITICAL(&s_lock);
    if (live_count < 1u || live_count > DBMAP_MIC_COUNT_MAX) {
        live_count = 1u;
    }

    cJSON *capabilities = cJSON_CreateObject();
    cJSON_AddStringToObject(capabilities, "role", "ear");
    cJSON_AddStringToObject(capabilities, "hardware_profile", cfg->identity.hardware_revision);

    cJSON *cap_mics = cJSON_CreateArray();
    for (uint8_t i = 0; i < live_count; ++i) {
        cJSON *cap_mic = cJSON_CreateObject();
        cJSON_AddNumberToObject(cap_mic, "index", (double)i);
        cJSON_AddStringToObject(cap_mic, "model", s_model->id);
        cJSON_AddStringToObject(cap_mic, "slot", i == 0 ? "left" : "right");
        cJSON_AddItemToArray(cap_mics, cap_mic);
    }
    cJSON_AddItemToObject(capabilities, "microphones", cap_mics);
    cJSON_AddItemToObject(state, "capabilities", capabilities);

    const char *transport =
        snaps[0].transport_status[0] ? snaps[0].transport_status : "unavailable";
    const char *mic_status =
        snaps[0].microphone_status[0] ? snaps[0].microphone_status : "unavailable";

    cJSON *audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "units", "dBFS");
    /* subsystem_status kept as transport alias for older Hub/UI consumers. */
    cJSON_AddStringToObject(audio, "subsystem_status", transport);
    cJSON_AddStringToObject(audio, "audio_transport_status", transport);
    cJSON_AddStringToObject(audio, "microphone_status", mic_status);
    cJSON_AddNumberToObject(audio, "sample_rate_hz", s_mic_cfg.sample_rate_hz);
    cJSON_AddStringToObject(audio, "sample_format", "int32_from_i2s32_left_justified_24");
    cJSON_AddStringToObject(audio, "slot_mode", s_slot_mode_name);
    cJSON_AddNumberToObject(audio, "slot_words_per_frame", (double)s_slot_words_per_frame);

    uint8_t configured = cfg->microphone_count;
    if (!dbmap_mic_count_supported(configured)) {
        configured = DBMAP_MIC_COUNT_DEFAULT;
    }
    cJSON_AddNumberToObject(audio, "configured_microphone_count", (double)configured);

    /* Available = delivered samples this window. Not the same as healthy. */
    uint8_t available = 0;
    for (uint8_t i = 0; i < live_count; ++i) {
        if (snaps[i].measured_at_us > 0
            && snaps[i].missing_data == 0
            && snaps[i].sample_count > 0) {
            available++;
        }
    }
    cJSON_AddNumberToObject(audio, "available_microphone_count", (double)available);
    cJSON_AddStringToObject(
        audio,
        "microphone_count_status",
        dbmap_mic_count_status_str(cfg->microphone_count_status)
    );
    cJSON_AddNumberToObject(audio, "incomplete_stereo_frames", (double)incomplete_stereo);
    /* DMA values actually configured at i2s_new_channel (measured config, not estimate). */
    cJSON_AddNumberToObject(audio, "dma_desc_num", (double)s_dma_desc_num);
    cJSON_AddNumberToObject(audio, "dma_frame_num", (double)s_dma_frame_num);
    cJSON_AddNumberToObject(audio, "slot_buf_bytes", (double)sizeof(s_slot_buf));
    cJSON_AddNumberToObject(audio, "i2s_request_bytes", (double)s_request_bytes);
    /* dma_buffer_bytes = dma_frame_num * slot_words * 4 — IDF formula from configured values. */
    const uint32_t dma_buf_bytes =
        s_dma_frame_num * s_slot_words_per_frame * (uint32_t)sizeof(uint32_t);
    cJSON_AddNumberToObject(audio, "dma_buffer_bytes_formula", (double)dma_buf_bytes);
    cJSON_AddNumberToObject(audio, "measure_window_us", (double)s_measure_window_us_last);

    cJSON *mics = cJSON_CreateArray();
    for (uint8_t i = 0; i < live_count; ++i) {
        cJSON *mic = cJSON_CreateObject();
        enrich_mic_object(mic, &snaps[i], i);
#if CONFIG_DBMAP_EAR_MIC_BACKGROUND
        if (i == 0) {
            /* Background RMS remains mic-0 only (Iteration 1). */
            dbmap_bg_rms_profile_t bg;
            portENTER_CRITICAL(&s_lock);
            dbmap_bg_rms_profile(&s_bg_hist, esp_timer_get_time(), &bg);
            portEXIT_CRITICAL(&s_lock);
            if (bg.present) {
                cJSON *background = cJSON_CreateObject();
                if (background != NULL) {
                    cJSON_AddNumberToObject(background, "window_ms", (double)bg.window_ms);
                    cJSON_AddNumberToObject(background, "sample_count", (double)bg.sample_count);
                    cJSON_AddStringToObject(background, "status", bg.status);
                    cJSON_AddNumberToObject(background, "rms_min_dbfs", (double)bg.rms_min_dbfs);
                    cJSON_AddNumberToObject(background, "rms_max_dbfs", (double)bg.rms_max_dbfs);
                    cJSON_AddNumberToObject(background, "rms_mean_dbfs", (double)bg.rms_mean_dbfs);
                    cJSON_AddItemToObject(mic, "background", background);
                }
            }
        }
#endif
        cJSON_AddItemToArray(mics, mic);
    }
    cJSON_AddItemToObject(audio, "microphones", mics);
    cJSON_AddItemToObject(state, "audio", audio);
}

static esp_err_t init_i2s_rx(void)
{
    if (s_rx_ready) {
        return ESP_OK;
    }
    if (s_mic_cfg.bclk_gpio < 0 || s_mic_cfg.ws_gpio < 0 || s_mic_cfg.din_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool stereo = (s_live_mic_count >= 2u);
    s_slot_words_per_frame = dbmap_i2s_slot_words_per_frame(s_live_mic_count);
    s_request_bytes =
        (uint32_t)EAR_MIC_READ_FRAMES * s_slot_words_per_frame * (uint32_t)sizeof(uint32_t);
    if (s_request_bytes > sizeof(s_slot_buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    strncpy(
        s_slot_mode_name,
        stereo ? "stereo_lr" : "mono_left",
        sizeof(s_slot_mode_name) - 1
    );
    s_slot_mode_name[sizeof(s_slot_mode_name) - 1] = '\0';

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* auto_clear_* applies only to TX channels in ESP-IDF 5.4.x (see i2s_new_channel).
     * This image opens RX only (tx_handle=NULL), so the flag is unused; keep false explicitly. */
    chan_cfg.auto_clear = false;
    /* Record values actually passed (IDF default macro: desc=6, frame=240 unless overridden). */
    s_dma_desc_num = chan_cfg.dma_desc_num;
    s_dma_frame_num = chan_cfg.dma_frame_num;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "new rx");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_mic_cfg.sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT,
            stereo ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO
        ),
        .gpio_cfg = {
            .mclk = gpio_or_unused(s_mic_cfg.mclk_gpio),
            .bclk = s_mic_cfg.bclk_gpio,
            .ws = s_mic_cfg.ws_gpio,
            .dout = I2S_GPIO_UNUSED,
            .din = s_mic_cfg.din_gpio,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    /*
     * ESP32-S3 PHILIPS default sets slot_mask=BOTH even for mono; override mono to LEFT.
     * Stereo: BOTH — RX buffer order (IDF docs): WS-low then WS-high → left then right.
     * Software contract: mic0=left (even), mic1=right (odd). Physical L/R straps unverified.
     */
    if (stereo) {
        std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    } else {
        std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    }
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable");
    /* DMA starts immediately; ear_mic_task begins measure_window with no warmup skip. */
    s_i2s_enabled_us = esp_timer_get_time();
    s_window_seq = 0;
    s_rx_ready = true;
    ESP_LOGW(
        TAG,
        "I2S RX ready model=%s mode=%s bclk=%d ws=%d din=%d rate=%d "
        "dma_desc=%lu dma_frame=%lu req_bytes=%lu (pins UNVERIFIED; "
        "stereo L/R map is IDF-doc contract, not HW proof)",
        s_model->id,
        s_slot_mode_name,
        s_mic_cfg.bclk_gpio,
        s_mic_cfg.ws_gpio,
        s_mic_cfg.din_gpio,
        s_mic_cfg.sample_rate_hz,
        (unsigned long)s_dma_desc_num,
        (unsigned long)s_dma_frame_num,
        (unsigned long)s_request_bytes
    );
    return ESP_OK;
}

typedef struct {
    double sum_sq;
    int32_t peak_abs;
    uint32_t pcm_count;
    uint32_t clipped;
    int32_t raw_min;
    int32_t raw_max;
    uint32_t zero_count;
    uint32_t full_scale_count;
    uint32_t unique_count;
    uint8_t unique_capped;
    uint32_t max_identical_run;
    uint32_t cur_identical_run;
    int32_t prev_sample;
    bool have_prev;
    uint32_t slot_min;
    uint32_t slot_max;
    uint32_t slot_zero;
    uint32_t slot_msb;
    bool have_slot;
} ear_ch_accum_t;

static void ch_accum_init(ear_ch_accum_t *a, uint8_t ch)
{
    memset(a, 0, sizeof(*a));
    a->raw_min = INT32_MAX;
    a->raw_max = INT32_MIN;
    a->slot_min = UINT32_MAX;
    unique_reset(ch);
}

static void ch_accum_slot(ear_ch_accum_t *a, uint32_t slot)
{
    if (!a->have_slot) {
        a->slot_min = slot;
        a->slot_max = slot;
        a->have_slot = true;
    } else {
        if (slot < a->slot_min) {
            a->slot_min = slot;
        }
        if (slot > a->slot_max) {
            a->slot_max = slot;
        }
    }
    if (slot == 0u) {
        a->slot_zero++;
    }
    if ((slot & 0x80000000u) != 0u) {
        a->slot_msb++;
    }
}

static void ch_accum_sample(
    ear_ch_accum_t *a,
    uint8_t ch,
    int32_t sample,
    int32_t clip_threshold,
    int32_t full_scale_mag
)
{
    double v = (double)sample;
    a->sum_sq += v * v;
    int32_t abs_v = sample < 0
        ? (sample == INT32_MIN ? INT32_MAX : -sample)
        : sample;
    if (abs_v > a->peak_abs) {
        a->peak_abs = abs_v;
    }
    if (clip_threshold > 0 && abs_v >= clip_threshold) {
        a->clipped++;
    }
    if (sample < a->raw_min) {
        a->raw_min = sample;
    }
    if (sample > a->raw_max) {
        a->raw_max = sample;
    }
    if (sample == 0) {
        a->zero_count++;
    }
    if (full_scale_mag > 0 && abs_v >= full_scale_mag) {
        a->full_scale_count++;
    }
    unique_add(ch, sample, &a->unique_count, &a->unique_capped);
    if (!a->have_prev || sample != a->prev_sample) {
        a->cur_identical_run = 1;
        a->prev_sample = sample;
        a->have_prev = true;
    } else {
        a->cur_identical_run++;
    }
    if (a->cur_identical_run > a->max_identical_run) {
        a->max_identical_run = a->cur_identical_run;
    }
    a->pcm_count++;
}

static void finalize_channel(
    ear_audio_snapshot_t *out,
    const ear_ch_accum_t *a,
    uint32_t expected,
    uint32_t window_ms,
    int useful_bits,
    uint32_t read_errors,
    uint32_t timeout_reads,
    uint32_t empty_reads,
    uint8_t counters_capped,
    uint32_t read_calls,
    uint32_t bytes_received,
    uint32_t partial_reads,
    uint32_t window_seq,
    uint32_t ms_since_i2s_enable,
    int64_t measured_at_us
)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->transport_status, "unavailable", sizeof(out->transport_status) - 1);
    strncpy(out->microphone_status, "unavailable", sizeof(out->microphone_status) - 1);
    strncpy(out->channel_presence, "no_samples", sizeof(out->channel_presence) - 1);

    out->window_ms = window_ms;
    out->window_seq = window_seq;
    out->ms_since_i2s_enable = ms_since_i2s_enable;
    out->read_calls = read_calls;
    out->bytes_received = bytes_received;
    out->partial_reads = partial_reads;
    out->read_errors = read_errors;
    out->timeout_reads = timeout_reads;
    out->empty_reads = empty_reads;
    out->counters_capped = counters_capped;
    out->measured_at_us = measured_at_us;
    out->raw_slot_min = a->have_slot ? a->slot_min : 0;
    out->raw_slot_max = a->have_slot ? a->slot_max : 0;
    out->slot_zero_count = a->slot_zero;
    out->slot_msb_set_count = a->slot_msb;
    out->complete_frames = a->pcm_count;
    out->sample_count = a->pcm_count;
    out->clipped_samples = a->clipped;

    if (a->pcm_count == 0) {
        out->missing_data = 1;
        out->rms_dbfs = -120.0f;
        out->peak_dbfs = -120.0f;
        out->raw_min = 0;
        out->raw_max = 0;
        out->all_zero_window = 0;
        set_status(out, expected);
        const dbmap_ear_channel_presence_t p =
            dbmap_ear_channel_presence(0, 0, 0);
        strncpy(
            out->channel_presence,
            dbmap_ear_channel_presence_str(p),
            sizeof(out->channel_presence) - 1
        );
        return;
    }

    out->raw_min = a->raw_min;
    out->raw_max = a->raw_max;
    out->zero_sample_count = a->zero_count;
    out->full_scale_sample_count = a->full_scale_count;
    out->unique_sample_count = a->unique_count;
    out->consecutive_identical_samples = a->max_identical_run;
    out->unique_capped = a->unique_capped;
    out->all_zero_window = (a->zero_count >= a->pcm_count) ? 1u : 0u;

    int32_t rms_proxy = (int32_t)(sqrt(a->sum_sq / (double)a->pcm_count) + 0.5);
    out->rms_dbfs = dbmap_audio_rms_dbfs(&rms_proxy, 1, useful_bits);
    out->peak_dbfs = dbmap_audio_peak_dbfs(&a->peak_abs, 1, useful_bits);
    set_status(out, expected);

    const dbmap_ear_channel_presence_t presence = dbmap_ear_channel_presence(
        out->sample_count,
        out->zero_sample_count,
        out->clipped_samples
    );
    strncpy(
        out->channel_presence,
        dbmap_ear_channel_presence_str(presence),
        sizeof(out->channel_presence) - 1
    );
}

/**
 * Fill out[0..live_mic_count). Shared transport counters are identical across channels;
 * per-channel RMS/peak/clip/presence differ. L0 PCM stores mic0/left only.
 */
static void measure_window(ear_audio_snapshot_t *out, uint8_t live_count)
{
    const uint32_t window_ms = (uint32_t)CONFIG_DBMAP_EAR_MIC_WINDOW_MS;
    const uint32_t expected = dbmap_audio_expected_samples(
        (uint32_t)s_mic_cfg.sample_rate_hz,
        window_ms
    );
    const int64_t t_measure0 = esp_timer_get_time();

    if (live_count < 1u || live_count > DBMAP_MIC_COUNT_MAX) {
        live_count = 1u;
    }
    for (uint8_t i = 0; i < DBMAP_MIC_COUNT_MAX; ++i) {
        memset(&out[i], 0, sizeof(out[i]));
        strncpy(out[i].transport_status, "unavailable", sizeof(out[i].transport_status) - 1);
        strncpy(out[i].microphone_status, "unavailable", sizeof(out[i].microphone_status) - 1);
        strncpy(out[i].channel_presence, "no_samples", sizeof(out[i].channel_presence) - 1);
        out[i].window_ms = window_ms;
        out[i].rms_dbfs = -120.0f;
        out[i].peak_dbfs = -120.0f;
    }

    if (!s_rx_ready || s_model == NULL) {
        for (uint8_t i = 0; i < live_count; ++i) {
            out[i].missing_data = 1;
            set_status(&out[i], expected);
        }
        s_measure_window_us_last = (uint32_t)(esp_timer_get_time() - t_measure0);
        return;
    }

    const bool stereo = (live_count >= 2u);
    const int useful_bits = s_model->useful_bits;
    const double full_scale = (double)(1LL << (useful_bits > 1 ? useful_bits - 1 : 1));
    const int32_t clip_threshold = (int32_t)(full_scale * 0.99);
    const int32_t full_scale_mag = (int32_t)full_scale;
    const size_t request_bytes = s_request_bytes > 0 ? s_request_bytes : sizeof(s_slot_buf);
    const int64_t deadline_us = t_measure0 + ((int64_t)window_ms * 1000LL);

    ear_ch_accum_t acc[DBMAP_MIC_COUNT_MAX];
    for (uint8_t i = 0; i < live_count; ++i) {
        ch_accum_init(&acc[i], i);
    }

    uint32_t read_errors = 0;
    uint32_t timeout_reads = 0;
    uint32_t empty_reads = 0;
    uint8_t counters_capped = 0;
    uint32_t read_calls = 0;
    uint32_t bytes_received = 0;
    uint32_t partial_reads = 0;
    uint32_t incomplete_stereo = 0;

    while (esp_timer_get_time() < deadline_us) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(
            s_rx,
            s_slot_buf,
            request_bytes,
            &bytes_read,
            50
        );
        read_calls++;
        bytes_received += (uint32_t)bytes_read;
        if (bytes_read > 0 && bytes_read < request_bytes) {
            partial_reads++;
        }

        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            bump_counter(&read_errors, &counters_capped);
            taskYIELD();
            continue;
        }

        const size_t slot_words = bytes_read / sizeof(uint32_t);
        if (stereo) {
            incomplete_stereo += dbmap_i2s_stereo_incomplete_trailing(slot_words);
            const size_t pairs = dbmap_i2s_stereo_complete_pairs(slot_words);
            for (size_t p = 0; p < pairs; ++p) {
                const uint32_t left_slot = s_slot_buf[p * 2u];
                const uint32_t right_slot = s_slot_buf[p * 2u + 1u];
                ch_accum_slot(&acc[0], left_slot);
                ch_accum_slot(&acc[1], right_slot);
                const int32_t left = s_model->extract(left_slot);
                const int32_t right = s_model->extract(right_slot);
                ch_accum_sample(&acc[0], 0, left, clip_threshold, full_scale_mag);
                ch_accum_sample(&acc[1], 1, right, clip_threshold, full_scale_mag);
#if CONFIG_DBMAP_EAR_MIC_L0
                /* L0 / Observation remain mic0 (left) only. */
                if (s_l0_pcm_ready && s_l0_pcm[s_l0_fill_idx] != NULL
                    && acc[0].pcm_count <= s_l0_pcm_cap
                    && (acc[0].pcm_count - 1u) < s_l0_pcm_cap) {
                    s_l0_pcm[s_l0_fill_idx][acc[0].pcm_count - 1u] = left;
                }
#endif
            }
        } else {
            for (size_t i = 0; i < slot_words; ++i) {
                const uint32_t slot = s_slot_buf[i];
                ch_accum_slot(&acc[0], slot);
                const int32_t sample = s_model->extract(slot);
                ch_accum_sample(&acc[0], 0, sample, clip_threshold, full_scale_mag);
#if CONFIG_DBMAP_EAR_MIC_L0
                if (s_l0_pcm_ready && s_l0_pcm[s_l0_fill_idx] != NULL
                    && acc[0].pcm_count <= s_l0_pcm_cap
                    && (acc[0].pcm_count - 1u) < s_l0_pcm_cap) {
                    s_l0_pcm[s_l0_fill_idx][acc[0].pcm_count - 1u] = sample;
                }
#endif
            }
        }

        if (err == ESP_ERR_TIMEOUT) {
            bump_counter(&timeout_reads, &counters_capped);
            continue;
        }
        if (slot_words == 0) {
            bump_counter(&empty_reads, &counters_capped);
            taskYIELD();
            continue;
        }
    }

    if (s_window_seq < UINT32_MAX) {
        s_window_seq++;
    }
    uint32_t ms_since = 0;
    if (s_i2s_enabled_us > 0) {
        int64_t elapsed_ms = (esp_timer_get_time() - s_i2s_enabled_us) / 1000;
        ms_since = elapsed_ms > 0 ? (uint32_t)elapsed_ms : 0;
    }
    const int64_t measured_at = esp_timer_get_time();
    s_incomplete_stereo_frames_window = incomplete_stereo;

    for (uint8_t i = 0; i < live_count; ++i) {
        finalize_channel(
            &out[i],
            &acc[i],
            expected,
            window_ms,
            useful_bits,
            read_errors,
            timeout_reads,
            empty_reads,
            counters_capped,
            read_calls,
            bytes_received,
            partial_reads,
            s_window_seq,
            ms_since,
            measured_at
        );
    }

#if CONFIG_DBMAP_EAR_MIC_L0
    if (s_l0_pcm_ready && acc[0].pcm_count > 0) {
        const size_t stored =
            acc[0].pcm_count < s_l0_pcm_cap ? acc[0].pcm_count : s_l0_pcm_cap;
        portENTER_CRITICAL(&s_lock);
        if (s_l0_pending.pending) {
            s_l0_pending_overwrite++;
        }
        s_l0_pending.pending = true;
        s_l0_pending.buf_idx = s_l0_fill_idx;
        s_l0_pending.sample_count = stored;
        s_l0_pending.window_ms = window_ms;
        s_l0_pending.window_end_mono_us = measured_at;
        s_l0_pending.full_scale_bits = useful_bits;
        s_l0_fill_idx ^= 1;
        portEXIT_CRITICAL(&s_lock);
    }
#endif

    s_measure_window_us_last = (uint32_t)(esp_timer_get_time() - t_measure0);

    if (out[0].window_seq <= 30u) {
        ESP_LOGI(
            TAG,
            "win#%lu mode=%s t+%lums reads=%lu bytes=%lu ch0_n=%lu ch1_n=%lu "
            "timeout=%lu incomplete=%lu rms0=%.1f rms1=%.1f peak0=%.1f peak1=%.1f "
            "pres0=%s pres1=%s measure_us=%lu",
            (unsigned long)out[0].window_seq,
            s_slot_mode_name,
            (unsigned long)out[0].ms_since_i2s_enable,
            (unsigned long)out[0].read_calls,
            (unsigned long)out[0].bytes_received,
            (unsigned long)out[0].sample_count,
            (unsigned long)(live_count > 1 ? out[1].sample_count : 0),
            (unsigned long)out[0].timeout_reads,
            (unsigned long)incomplete_stereo,
            (double)out[0].rms_dbfs,
            (double)(live_count > 1 ? out[1].rms_dbfs : -120.0f),
            (double)out[0].peak_dbfs,
            (double)(live_count > 1 ? out[1].peak_dbfs : -120.0f),
            out[0].channel_presence,
            live_count > 1 ? out[1].channel_presence : "n/a",
            (unsigned long)s_measure_window_us_last
        );
    }
}

#if CONFIG_DBMAP_EAR_MIC_CAPTURE_DIAG
static void capture_diag_maybe_log(const ear_audio_snapshot_t *snaps, uint8_t live_count)
{
    const int64_t now = esp_timer_get_time();
    if (s_capture_diag_last_us != 0 && (now - s_capture_diag_last_us) < 5000000LL) {
        return;
    }
    s_capture_diag_last_us = now;

    const uint32_t dma_buf_bytes =
        s_dma_frame_num * s_slot_words_per_frame * (uint32_t)sizeof(uint32_t);
    /* Mono comparison: same DMA frame count with 1 slot word — configured formula, not a
     * second live measurement on this boot. */
    const uint32_t mono_dma_buf_bytes =
        s_dma_frame_num * 1u * (uint32_t)sizeof(uint32_t);
    const uint32_t mono_slot_buf_bytes =
        (uint32_t)EAR_MIC_READ_FRAMES * (uint32_t)sizeof(uint32_t);
    const uint32_t free_heap = (uint32_t)esp_get_free_heap_size();
    const uint32_t free_spiram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(
        TAG,
        DBMAP_EAR_MIC_CAPTURE_DIAG_SUMMARY_FMT,
        s_mic_cfg.sample_rate_hz,
        s_slot_mode_name,
        (unsigned long)s_slot_words_per_frame,
        (unsigned)live_count,
        (unsigned long)s_dma_desc_num,
        (unsigned long)s_dma_frame_num,
        (unsigned long)dma_buf_bytes,
        (unsigned long)mono_dma_buf_bytes,
        (unsigned long)sizeof(s_slot_buf),
        (unsigned long)mono_slot_buf_bytes,
        (unsigned long)s_request_bytes,
        (unsigned long)s_measure_window_us_last,
        (unsigned long)s_incomplete_stereo_frames_window,
        (unsigned long)free_heap,
        (unsigned long)free_spiram
    );
    for (uint8_t i = 0; i < live_count; ++i) {
        ESP_LOGI(
            TAG,
            DBMAP_EAR_MIC_CAPTURE_DIAG_CH_FMT,
            (unsigned)i,
            i == 0 ? "left" : "right",
            (unsigned long)snaps[i].sample_count,
            (unsigned long)dbmap_audio_expected_samples(
                (uint32_t)s_mic_cfg.sample_rate_hz,
                snaps[i].window_ms
            ),
            (double)snaps[i].rms_dbfs,
            (double)snaps[i].peak_dbfs,
            (unsigned long)snaps[i].clipped_samples,
            (unsigned long)snaps[i].zero_sample_count,
            (unsigned)snaps[i].all_zero_window,
            snaps[i].channel_presence,
            snaps[i].microphone_status[0] ? snaps[i].microphone_status : "unavailable",
            snaps[i].transport_status[0] ? snaps[i].transport_status : "unavailable",
            (unsigned long)snaps[i].timeout_reads,
            (unsigned long)snaps[i].empty_reads,
            (unsigned long)snaps[i].read_errors,
            (unsigned long)snaps[i].window_seq
        );
    }
}
#endif

static void ear_mic_task(void *arg)
{
    (void)arg;
    while (1) {
        ear_audio_snapshot_t snaps[DBMAP_MIC_COUNT_MAX];
        measure_window(snaps, s_live_mic_count);

#if CONFIG_DBMAP_EAR_MIC_BACKGROUND
        dbmap_bg_rms_candidate_t cand = {
            .levels_valid = (snaps[0].missing_data == 0 && snaps[0].sample_count > 0),
            .rms_dbfs = snaps[0].rms_dbfs,
            .peak_dbfs = snaps[0].peak_dbfs,
            .clipped_samples = snaps[0].clipped_samples,
            .window_seq = snaps[0].window_seq,
            .ms_since_i2s_enable = snaps[0].ms_since_i2s_enable,
            .transport_status = snaps[0].transport_status,
            .microphone_status = snaps[0].microphone_status,
        };
        const bool include = dbmap_bg_rms_should_include(
            &cand, (uint32_t)CONFIG_DBMAP_EAR_MIC_BACKGROUND_BOOT_SETTLE_MS
        );
#endif

        portENTER_CRITICAL(&s_lock);
        for (uint8_t i = 0; i < DBMAP_MIC_COUNT_MAX; ++i) {
            s_latest[i] = snaps[i];
        }
#if CONFIG_DBMAP_EAR_MIC_BACKGROUND
        if (include) {
            dbmap_bg_rms_push(&s_bg_hist, snaps[0].rms_dbfs, snaps[0].measured_at_us);
        }
#endif
        portEXIT_CRITICAL(&s_lock);

#if CONFIG_DBMAP_EAR_MIC_CAPTURE_DIAG
        capture_diag_maybe_log(snaps, s_live_mic_count);
#endif

        /* Yield so MQTT / OTA / health stay responsive. Latest levels ride on hello/health. */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t dbmap_ear_mic_start(const dbmap_effective_t *cfg, const dbmap_ear_mic_config_t *mic_cfg)
{
    if (cfg == NULL || mic_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cfg != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_model = dbmap_ear_mic_model_desc(mic_cfg->model);
    if (s_model == NULL) {
        s_model = dbmap_ear_mic_model_selected();
    }
    if (s_model == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_cfg = cfg;
    s_mic_cfg = *mic_cfg;
    if (s_mic_cfg.sample_rate_hz <= 0) {
        s_mic_cfg.sample_rate_hz = CONFIG_DBMAP_EAR_MIC_SAMPLE_RATE_HZ;
    }

    s_live_mic_count = cfg->microphone_count;
    if (!dbmap_mic_count_supported(s_live_mic_count)) {
        s_live_mic_count = DBMAP_MIC_COUNT_DEFAULT;
    }
    ESP_LOGI(
        TAG,
        "live_mic_count=%u (NVS configured=%u status=%s) → %s",
        (unsigned)s_live_mic_count,
        (unsigned)cfg->microphone_count,
        dbmap_mic_count_status_str(cfg->microphone_count_status),
        s_live_mic_count >= 2u ? "stereo_lr" : "mono_left"
    );
#if CONFIG_DBMAP_EAR_MIC_CAPTURE_DIAG
    ESP_LOGI(TAG, "%s", DBMAP_EAR_MIC_CAPTURE_DIAG_BOOT_MSG);
#endif

    memset(s_latest, 0, sizeof(s_latest));
    for (uint8_t i = 0; i < DBMAP_MIC_COUNT_MAX; ++i) {
        strncpy(
            s_latest[i].transport_status,
            "unavailable",
            sizeof(s_latest[i].transport_status) - 1
        );
        strncpy(
            s_latest[i].microphone_status,
            "unavailable",
            sizeof(s_latest[i].microphone_status) - 1
        );
        strncpy(
            s_latest[i].channel_presence,
            "no_samples",
            sizeof(s_latest[i].channel_presence) - 1
        );
    }

#if CONFIG_DBMAP_EAR_MIC_BACKGROUND
    dbmap_bg_rms_hist_init(
        &s_bg_hist,
        s_bg_entries,
        EAR_BG_RMS_CAP,
        (uint32_t)CONFIG_DBMAP_EAR_MIC_BACKGROUND_WINDOW_MS
    );
    ESP_LOGI(
        TAG,
        "background RMS hist window_ms=%lu cap=%lu boot_settle_ms=%lu",
        (unsigned long)CONFIG_DBMAP_EAR_MIC_BACKGROUND_WINDOW_MS,
        (unsigned long)EAR_BG_RMS_CAP,
        (unsigned long)CONFIG_DBMAP_EAR_MIC_BACKGROUND_BOOT_SETTLE_MS
    );
#endif

#if CONFIG_DBMAP_EAR_MIC_L0
    {
        const uint32_t window_ms = (uint32_t)CONFIG_DBMAP_EAR_MIC_WINDOW_MS;
        const uint32_t expected = dbmap_audio_expected_samples(
            (uint32_t)s_mic_cfg.sample_rate_hz,
            window_ms
        );
        /* +1% slack for slight over-fill vs nominal window length. */
        s_l0_pcm_cap = expected + (expected / 100u) + 16u;
        s_l0_pcm[0] = (int32_t *)heap_caps_malloc(
            (size_t)s_l0_pcm_cap * sizeof(int32_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        s_l0_pcm[1] = (int32_t *)heap_caps_malloc(
            (size_t)s_l0_pcm_cap * sizeof(int32_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (s_l0_pcm[0] == NULL || s_l0_pcm[1] == NULL) {
            ESP_LOGE(TAG, "L0 SPIRAM PCM alloc failed cap=%lu", (unsigned long)s_l0_pcm_cap);
            heap_caps_free(s_l0_pcm[0]);
            heap_caps_free(s_l0_pcm[1]);
            s_l0_pcm[0] = NULL;
            s_l0_pcm[1] = NULL;
            s_l0_pcm_ready = false;
        } else {
            s_l0_pcm_ready = true;
            s_l0_fill_idx = 0;
            memset(&s_l0_pending, 0, sizeof(s_l0_pending));
            memset(&s_l0_latest, 0, sizeof(s_l0_latest));
            s_l0_latest_ready = false;
            dbmap_l0_filter_bank_init(&s_l0_bank, (uint32_t)s_mic_cfg.sample_rate_hz);
            s_l0_free_heap_after_init = (uint32_t)esp_get_free_heap_size();
            s_l0_free_spiram_after_init = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            ESP_LOGI(
                TAG,
                "L0 ready pcm_cap=%lu bytes=%lu free_heap=%lu free_spiram=%lu",
                (unsigned long)s_l0_pcm_cap,
                (unsigned long)(s_l0_pcm_cap * sizeof(int32_t) * 2u),
                (unsigned long)s_l0_free_heap_after_init,
                (unsigned long)s_l0_free_spiram_after_init
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
    }
#endif

    esp_err_t err = init_i2s_rx();
    if (err != ESP_OK) {
        for (uint8_t i = 0; i < DBMAP_MIC_COUNT_MAX; ++i) {
            strncpy(
                s_latest[i].transport_status,
                "error",
                sizeof(s_latest[i].transport_status) - 1
            );
            strncpy(
                s_latest[i].microphone_status,
                "unavailable",
                sizeof(s_latest[i].microphone_status) - 1
            );
        }
        dbmap_health_set_enricher(enrich_health);
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        return err;
    }

    dbmap_health_set_enricher(enrich_health);

    BaseType_t ok = xTaskCreate(ear_mic_task, "ear_mic", EAR_MIC_TASK_STACK, NULL, 5, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
