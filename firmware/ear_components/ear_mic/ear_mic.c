#include "dbmap_ear_mic.h"

#include <math.h>
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
    uint32_t read_errors;
    uint32_t stalled_reads;
    uint32_t missing_data;
    uint32_t clipped_samples;
    char status[16];
    int64_t measured_at_us;
} ear_audio_snapshot_t;

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
    strncpy(s_status_cache, s_latest.status[0] ? s_latest.status : "unavailable", sizeof(s_status_cache) - 1);
    s_status_cache[sizeof(s_status_cache) - 1] = '\0';
    portEXIT_CRITICAL(&s_lock);
    return s_status_cache;
}

static void set_status(ear_audio_snapshot_t *snap)
{
    if (snap->sample_count == 0 || snap->missing_data > 0) {
        strncpy(snap->status, "error", sizeof(snap->status) - 1);
    } else if (snap->read_errors > 0 || snap->stalled_reads > 0) {
        strncpy(snap->status, "degraded", sizeof(snap->status) - 1);
    } else {
        strncpy(snap->status, "ok", sizeof(snap->status) - 1);
    }
    snap->status[sizeof(snap->status) - 1] = '\0';
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

    cJSON *audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "units", "dBFS");
    cJSON_AddStringToObject(audio, "subsystem_status", snap.status[0] ? snap.status : "unavailable");
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
    cJSON_AddNumberToObject(mic, "window_ms", snap.window_ms);
    cJSON_AddNumberToObject(mic, "read_errors", snap.read_errors);
    cJSON_AddNumberToObject(mic, "stalled_reads", snap.stalled_reads);
    cJSON_AddNumberToObject(mic, "missing_data", snap.missing_data);
    cJSON_AddNumberToObject(mic, "clipped_samples", snap.clipped_samples);
    cJSON_AddStringToObject(mic, "status", snap.status[0] ? snap.status : "unavailable");
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
    chan_cfg.auto_clear = true;
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
    strncpy(out->status, "unavailable", sizeof(out->status) - 1);

    if (!s_rx_ready || s_model == NULL) {
        out->missing_data = 1;
        set_status(out);
        return;
    }

    const uint32_t window_ms = (uint32_t)CONFIG_DBMAP_EAR_MIC_WINDOW_MS;
    const int64_t deadline_us = esp_timer_get_time() + ((int64_t)window_ms * 1000LL);
    const int useful_bits = s_model->useful_bits;
    const double full_scale = (double)(1LL << (useful_bits > 1 ? useful_bits - 1 : 1));
    const int32_t clip_threshold = (int32_t)(full_scale * 0.99);
    double sum_sq = 0.0;
    int32_t peak_abs = 0;
    uint32_t pcm_count = 0;
    uint32_t clipped = 0;

    /* Accumulate stats in-stream so a 1 s window does not need an 8 KiB PCM buffer. */
    while (esp_timer_get_time() < deadline_us) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(
            s_rx,
            s_slot_buf,
            sizeof(s_slot_buf),
            &bytes_read,
            pdMS_TO_TICKS(50)
        );
        if (err == ESP_ERR_TIMEOUT || (err == ESP_OK && bytes_read == 0)) {
            out->stalled_reads++;
            continue;
        }
        if (err != ESP_OK) {
            out->read_errors++;
            continue;
        }
        size_t frames = bytes_read / sizeof(uint32_t);
        for (size_t i = 0; i < frames; ++i) {
            int32_t sample = s_model->extract(s_slot_buf[i]);
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
            pcm_count++;
        }
    }

    out->window_ms = window_ms;
    out->sample_count = pcm_count;
    out->clipped_samples = clipped;
    out->measured_at_us = esp_timer_get_time();
    if (pcm_count == 0) {
        out->missing_data = 1;
        out->rms_dbfs = -120.0f;
        out->peak_dbfs = -120.0f;
        set_status(out);
        return;
    }

    /* Single-sample helpers for numerically consistent dBFS conversion. */
    int32_t rms_proxy = (int32_t)(sqrt(sum_sq / (double)pcm_count) + 0.5);
    out->rms_dbfs = dbmap_audio_rms_dbfs(&rms_proxy, 1, useful_bits);
    out->peak_dbfs = dbmap_audio_peak_dbfs(&peak_abs, 1, useful_bits);
    set_status(out);
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
    strncpy(s_latest.status, "unavailable", sizeof(s_latest.status) - 1);

    esp_err_t err = init_i2s_rx();
    if (err != ESP_OK) {
        strncpy(s_latest.status, "error", sizeof(s_latest.status) - 1);
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
