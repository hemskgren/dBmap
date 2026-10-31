#include "dbmap_ear_mic.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "dbmap_audio_metrics.h"
#include "dbmap_health.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic_model.h"
#include "sdkconfig.h"

static const char *TAG = "ear_mic";

#define EAR_MIC_READ_FRAMES 256
#define EAR_MIC_TASK_STACK 6144

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
    char transport_status[16]; /* I2S fill health: ok|degraded|error */
    char microphone_status[24]; /* unavailable|disconnected_suspected|noise_suspected|signal_present|unknown */
    char classification_reason[48];
    int64_t measured_at_us;
} ear_audio_snapshot_t;

/* Bounded unique-value table for window diagnostics (not a full histogram). */
#define EAR_UNIQUE_CAP 1024u
static int32_t s_unique_vals[EAR_UNIQUE_CAP];
static uint8_t s_unique_occ[EAR_UNIQUE_CAP];

static void unique_reset(void)
{
    memset(s_unique_occ, 0, sizeof(s_unique_occ));
}

static void unique_add(int32_t sample, uint32_t *unique_count, uint8_t *unique_capped)
{
    if (unique_count == NULL || unique_capped == NULL) {
        return;
    }
    if (*unique_count >= EAR_UNIQUE_CAP) {
        *unique_capped = 1;
        return;
    }
    uint32_t hash = (uint32_t)sample * 2654435761u;
    for (uint32_t probe = 0; probe < EAR_UNIQUE_CAP; ++probe) {
        uint32_t idx = (hash + probe) % EAR_UNIQUE_CAP;
        if (s_unique_occ[idx] == 0) {
            s_unique_occ[idx] = 1;
            s_unique_vals[idx] = sample;
            (*unique_count)++;
            return;
        }
        if (s_unique_vals[idx] == sample) {
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
static ear_audio_snapshot_t s_latest;
static char s_status_cache[16] = "unavailable";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
/* Keep I2S read scratch off the task stack (was overflowing a 4 KiB stack → TG1WDT). */
static uint32_t s_slot_buf[EAR_MIC_READ_FRAMES];
static int64_t s_i2s_enabled_us;
static uint32_t s_window_seq;

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
        s_latest.transport_status[0] ? s_latest.transport_status : "unavailable",
        sizeof(s_status_cache) - 1
    );
    s_status_cache[sizeof(s_status_cache) - 1] = '\0';
    portEXIT_CRITICAL(&s_lock);
    return s_status_cache;
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
 * microphones[] is sized for future multi-mic Ears; this image reports index 0 only.
 */
static void enrich_health(cJSON *state, const dbmap_effective_t *cfg, bool include_diag)
{
    (void)include_diag;
    if (state == NULL || cfg == NULL || s_model == NULL) {
        return;
    }

    ear_audio_snapshot_t snap;
    portENTER_CRITICAL(&s_lock);
    snap = s_latest;
    portEXIT_CRITICAL(&s_lock);

    cJSON *capabilities = cJSON_CreateObject();
    cJSON_AddStringToObject(capabilities, "role", "ear");
    cJSON_AddStringToObject(capabilities, "hardware_profile", cfg->identity.hardware_revision);

    cJSON *cap_mics = cJSON_CreateArray();
    cJSON *cap_mic = cJSON_CreateObject();
    cJSON_AddNumberToObject(cap_mic, "index", 0);
    cJSON_AddStringToObject(cap_mic, "model", s_model->id);
    cJSON_AddItemToArray(cap_mics, cap_mic);
    cJSON_AddItemToObject(capabilities, "microphones", cap_mics);
    cJSON_AddItemToObject(state, "capabilities", capabilities);

    const char *transport =
        snap.transport_status[0] ? snap.transport_status : "unavailable";
    const char *mic_status =
        snap.microphone_status[0] ? snap.microphone_status : "unavailable";

    cJSON *audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "units", "dBFS");
    /* subsystem_status kept as transport alias for older Hub/UI consumers. */
    cJSON_AddStringToObject(audio, "subsystem_status", transport);
    cJSON_AddStringToObject(audio, "audio_transport_status", transport);
    cJSON_AddStringToObject(audio, "microphone_status", mic_status);
    cJSON_AddNumberToObject(audio, "sample_rate_hz", s_mic_cfg.sample_rate_hz);
    cJSON_AddStringToObject(audio, "sample_format", "int32_from_i2s32_left_justified_24");

    cJSON *mics = cJSON_CreateArray();
    cJSON *mic = cJSON_CreateObject();
    cJSON_AddNumberToObject(mic, "index", 0);
    cJSON_AddStringToObject(mic, "model", s_model->id);
    cJSON_AddNumberToObject(mic, "channels", 1);
    cJSON_AddNumberToObject(mic, "rms_dbfs", snap.rms_dbfs);
    cJSON_AddNumberToObject(mic, "peak_dbfs", snap.peak_dbfs);
    cJSON_AddNumberToObject(mic, "sample_count", snap.sample_count);
    cJSON_AddNumberToObject(
        mic,
        "expected_sample_count",
        dbmap_audio_expected_samples((uint32_t)s_mic_cfg.sample_rate_hz, snap.window_ms)
    );
    cJSON_AddNumberToObject(mic, "window_ms", snap.window_ms);
    cJSON_AddNumberToObject(mic, "read_errors", snap.read_errors);
    cJSON_AddNumberToObject(mic, "timeout_reads", snap.timeout_reads);
    cJSON_AddNumberToObject(mic, "empty_reads", snap.empty_reads);
    cJSON_AddNumberToObject(mic, "missing_data", snap.missing_data);
    cJSON_AddNumberToObject(mic, "clipped_samples", snap.clipped_samples);
    cJSON_AddBoolToObject(mic, "counters_capped", snap.counters_capped != 0);
    cJSON_AddNumberToObject(mic, "raw_min", snap.raw_min);
    cJSON_AddNumberToObject(mic, "raw_max", snap.raw_max);
    cJSON_AddNumberToObject(mic, "zero_sample_count", snap.zero_sample_count);
    cJSON_AddNumberToObject(mic, "full_scale_sample_count", snap.full_scale_sample_count);
    cJSON_AddNumberToObject(mic, "unique_sample_count", snap.unique_sample_count);
    cJSON_AddNumberToObject(mic, "consecutive_identical_samples", snap.consecutive_identical_samples);
    cJSON_AddBoolToObject(mic, "unique_capped", snap.unique_capped != 0);
    cJSON_AddNumberToObject(mic, "window_seq", snap.window_seq);
    cJSON_AddNumberToObject(mic, "ms_since_i2s_enable", snap.ms_since_i2s_enable);
    cJSON_AddNumberToObject(mic, "read_calls", snap.read_calls);
    cJSON_AddNumberToObject(mic, "bytes_received", snap.bytes_received);
    cJSON_AddNumberToObject(mic, "complete_frames", snap.complete_frames);
    cJSON_AddNumberToObject(mic, "partial_reads", snap.partial_reads);
    cJSON_AddNumberToObject(mic, "raw_slot_min", (double)snap.raw_slot_min);
    cJSON_AddNumberToObject(mic, "raw_slot_max", (double)snap.raw_slot_max);
    {
        char hex[11];
        snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)snap.raw_slot_min);
        cJSON_AddStringToObject(mic, "raw_slot_min_hex", hex);
        snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)snap.raw_slot_max);
        cJSON_AddStringToObject(mic, "raw_slot_max_hex", hex);
    }
    cJSON_AddNumberToObject(mic, "slot_zero_count", snap.slot_zero_count);
    cJSON_AddNumberToObject(mic, "slot_msb_set_count", snap.slot_msb_set_count);
    cJSON_AddStringToObject(mic, "status", transport);
    cJSON_AddStringToObject(mic, "microphone_status", mic_status);
    if (snap.classification_reason[0] != '\0') {
        cJSON_AddStringToObject(mic, "classification_reason", snap.classification_reason);
    }
    if (snap.measured_at_us > 0) {
        cJSON_AddNumberToObject(mic, "measured_at_uptime_s", (double)(snap.measured_at_us / 1000000LL));
    }
    cJSON_AddItemToArray(mics, mic);
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

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* auto_clear_* applies only to TX channels in ESP-IDF 5.4.x (see i2s_new_channel).
     * This image opens RX only (tx_handle=NULL), so the flag is unused; keep false explicitly. */
    chan_cfg.auto_clear = false;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "new rx");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_mic_cfg.sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
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
    /* INMP441 L/R tied to GND → left slot. Unverified until physical strap is checked. */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable");
    /* DMA starts immediately; ear_mic_task begins measure_window with no warmup skip. */
    s_i2s_enabled_us = esp_timer_get_time();
    s_window_seq = 0;
    s_rx_ready = true;
    ESP_LOGW(
        TAG,
        "I2S RX ready model=%s bclk=%d ws=%d din=%d rate=%d (pins UNVERIFIED)",
        s_model->id,
        s_mic_cfg.bclk_gpio,
        s_mic_cfg.ws_gpio,
        s_mic_cfg.din_gpio,
        s_mic_cfg.sample_rate_hz
    );
    return ESP_OK;
}

static void measure_window(ear_audio_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->transport_status, "unavailable", sizeof(out->transport_status) - 1);
    strncpy(out->microphone_status, "unavailable", sizeof(out->microphone_status) - 1);

    const uint32_t window_ms = (uint32_t)CONFIG_DBMAP_EAR_MIC_WINDOW_MS;
    const uint32_t expected = dbmap_audio_expected_samples(
        (uint32_t)s_mic_cfg.sample_rate_hz,
        window_ms
    );

    if (!s_rx_ready || s_model == NULL) {
        out->missing_data = 1;
        out->window_ms = window_ms;
        out->rms_dbfs = -120.0f;
        out->peak_dbfs = -120.0f;
        set_status(out, expected);
        return;
    }

    const int64_t deadline_us = esp_timer_get_time() + ((int64_t)window_ms * 1000LL);
    const int useful_bits = s_model->useful_bits;
    const double full_scale = (double)(1LL << (useful_bits > 1 ? useful_bits - 1 : 1));
    const int32_t clip_threshold = (int32_t)(full_scale * 0.99);
    const int32_t full_scale_mag = (int32_t)full_scale;
    double sum_sq = 0.0;
    int32_t peak_abs = 0;
    uint32_t pcm_count = 0;
    uint32_t clipped = 0;
    int32_t raw_min = INT32_MAX;
    int32_t raw_max = INT32_MIN;
    uint32_t zero_count = 0;
    uint32_t full_scale_count = 0;
    uint32_t unique_count = 0;
    uint8_t unique_capped = 0;
    uint32_t max_identical_run = 0;
    uint32_t cur_identical_run = 0;
    int32_t prev_sample = 0;
    bool have_prev = false;

    unique_reset();

    uint32_t read_calls = 0;
    uint32_t bytes_received = 0;
    uint32_t partial_reads = 0;
    uint32_t slot_min = UINT32_MAX;
    uint32_t slot_max = 0;
    uint32_t slot_zero = 0;
    uint32_t slot_msb = 0;
    bool have_slot = false;
    const size_t request_bytes = sizeof(s_slot_buf);

    /* Accumulate stats in-stream so a 1 s window does not need an 8 KiB PCM buffer.
     * Only frames covered by bytes_read are processed (driver memcpy into s_slot_buf).
     * Stale words beyond bytes_read are ignored. */
    while (esp_timer_get_time() < deadline_us) {
        size_t bytes_read = 0;
        /* timeout_ms is milliseconds: i2s_channel_read applies pdMS_TO_TICKS itself. */
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
            bump_counter(&out->read_errors, &out->counters_capped);
            taskYIELD();
            continue;
        }

        const size_t frames = bytes_read / sizeof(uint32_t);
        for (size_t i = 0; i < frames; ++i) {
            const uint32_t slot = s_slot_buf[i];
            if (!have_slot) {
                slot_min = slot;
                slot_max = slot;
                have_slot = true;
            } else {
                if (slot < slot_min) {
                    slot_min = slot;
                }
                if (slot > slot_max) {
                    slot_max = slot;
                }
            }
            if (slot == 0u) {
                slot_zero++;
            }
            if ((slot & 0x80000000u) != 0u) {
                slot_msb++;
            }

            /* INMP441 path: (int32_t)slot >> 8 — left-justified 24-bit, arithmetic sign extend. */
            int32_t sample = s_model->extract(slot);
            double v = (double)sample;
            sum_sq += v * v;
            int32_t abs_v = sample < 0
                ? (sample == INT32_MIN ? INT32_MAX : -sample)
                : sample;
            if (abs_v > peak_abs) {
                peak_abs = abs_v;
            }
            if (clip_threshold > 0 && abs_v >= clip_threshold) {
                clipped++;
            }
            if (sample < raw_min) {
                raw_min = sample;
            }
            if (sample > raw_max) {
                raw_max = sample;
            }
            if (sample == 0) {
                zero_count++;
            }
            if (full_scale_mag > 0 && abs_v >= full_scale_mag) {
                full_scale_count++;
            }
            unique_add(sample, &unique_count, &unique_capped);
            if (!have_prev || sample != prev_sample) {
                cur_identical_run = 1;
                prev_sample = sample;
                have_prev = true;
            } else {
                cur_identical_run++;
            }
            if (cur_identical_run > max_identical_run) {
                max_identical_run = cur_identical_run;
            }
            pcm_count++;
        }

        if (err == ESP_ERR_TIMEOUT) {
            bump_counter(&out->timeout_reads, &out->counters_capped);
            continue;
        }
        if (frames == 0) {
            bump_counter(&out->empty_reads, &out->counters_capped);
            taskYIELD();
            continue;
        }
    }

    if (s_window_seq < UINT32_MAX) {
        s_window_seq++;
    }
    out->window_seq = s_window_seq;
    if (s_i2s_enabled_us > 0) {
        int64_t elapsed_ms = (esp_timer_get_time() - s_i2s_enabled_us) / 1000;
        out->ms_since_i2s_enable = elapsed_ms > 0 ? (uint32_t)elapsed_ms : 0;
    }
    out->read_calls = read_calls;
    out->bytes_received = bytes_received;
    out->complete_frames = pcm_count;
    out->partial_reads = partial_reads;
    out->raw_slot_min = have_slot ? slot_min : 0;
    out->raw_slot_max = have_slot ? slot_max : 0;
    out->slot_zero_count = slot_zero;
    out->slot_msb_set_count = slot_msb;

    out->window_ms = window_ms;
    out->sample_count = pcm_count;
    out->clipped_samples = clipped;
    out->measured_at_us = esp_timer_get_time();
    if (pcm_count == 0) {
        out->missing_data = 1;
        out->rms_dbfs = -120.0f;
        out->peak_dbfs = -120.0f;
        out->raw_min = 0;
        out->raw_max = 0;
        set_status(out, expected);
        return;
    }

    out->raw_min = raw_min;
    out->raw_max = raw_max;
    out->zero_sample_count = zero_count;
    out->full_scale_sample_count = full_scale_count;
    out->unique_sample_count = unique_count;
    out->consecutive_identical_samples = max_identical_run;
    out->unique_capped = unique_capped;

    /* Single-sample helpers for numerically consistent dBFS conversion.
     * peak_dbfs == 0 means at least one |sample| >= 2^(useful_bits-1). */
    int32_t rms_proxy = (int32_t)(sqrt(sum_sq / (double)pcm_count) + 0.5);
    out->rms_dbfs = dbmap_audio_rms_dbfs(&rms_proxy, 1, useful_bits);
    out->peak_dbfs = dbmap_audio_peak_dbfs(&peak_abs, 1, useful_bits);
    set_status(out, expected);

    /* Bounded startup log: first 30 windows only (≈30 s at 1 s windows). */
    if (out->window_seq <= 30u) {
        ESP_LOGI(
            TAG,
            "win#%lu t+%lums reads=%lu bytes=%lu frames=%lu partial=%lu timeout=%lu "
            "zero=%lu fs=%lu slot_msb=%lu slot=[0x%08lx..0x%08lx] rms=%.1f peak=%.1f",
            (unsigned long)out->window_seq,
            (unsigned long)out->ms_since_i2s_enable,
            (unsigned long)out->read_calls,
            (unsigned long)out->bytes_received,
            (unsigned long)out->complete_frames,
            (unsigned long)out->partial_reads,
            (unsigned long)out->timeout_reads,
            (unsigned long)out->zero_sample_count,
            (unsigned long)out->full_scale_sample_count,
            (unsigned long)out->slot_msb_set_count,
            (unsigned long)out->raw_slot_min,
            (unsigned long)out->raw_slot_max,
            (double)out->rms_dbfs,
            (double)out->peak_dbfs
        );
    }
}

static void ear_mic_task(void *arg)
{
    (void)arg;
    while (1) {
        ear_audio_snapshot_t snap;
        measure_window(&snap);

        portENTER_CRITICAL(&s_lock);
        s_latest = snap;
        portEXIT_CRITICAL(&s_lock);

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

    memset(&s_latest, 0, sizeof(s_latest));
    strncpy(s_latest.transport_status, "unavailable", sizeof(s_latest.transport_status) - 1);
    strncpy(s_latest.microphone_status, "unavailable", sizeof(s_latest.microphone_status) - 1);

    esp_err_t err = init_i2s_rx();
    if (err != ESP_OK) {
        strncpy(s_latest.transport_status, "error", sizeof(s_latest.transport_status) - 1);
        strncpy(s_latest.microphone_status, "unavailable", sizeof(s_latest.microphone_status) - 1);
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
