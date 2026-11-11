#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sp_mic_channel_api.h"
#include "sp_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One microphone model per Ear build. Additional models can be added later;
 * Kconfig selects exactly one active model for a given firmware image.
 */
typedef enum {
    SP_MIC_MODEL_INMP441 = 1,
} sp_mic_model_t;

typedef struct {
    sp_mic_model_t model;
    int bclk_gpio;
    int ws_gpio;
    int din_gpio;
    int mclk_gpio; /* -1 if unused */
    int sample_rate_hz;
} sp_mic_config_t;

/**
 * Latest windowed RMS snapshot for Observation / activity detection.
 * transport_status and microphone_status are diagnostic gates only — not
 * Observation source_family classification.
 */
typedef struct {
    bool valid;
    float rms_dbfs;
    float peak_dbfs; /* same window as rms_dbfs; digital FS, not SPL */
    uint32_t window_ms;
    int64_t measured_at_us; /* esp_timer monotonic microseconds */
    uint32_t window_seq;
    uint32_t ms_since_i2s_enable;
    char transport_status[16];
    char microphone_status[24];
} sp_mic_levels_t;

/**
 * Start the bounded I2S acquisition + telemetry task.
 * Registers a hello/health enricher that publishes microphone capabilities.
 * cfg must remain valid for the lifetime of the task (app_main static config).
 */
esp_err_t sp_mic_start(const sp_effective_t *cfg, const sp_mic_config_t *mic_cfg);

/** Active model id string, e.g. "INMP441". */
const char *sp_mic_model_id(void);

/** Latest subsystem status string: ok | degraded | error | unavailable. */
const char *sp_mic_subsystem_status(void);

/**
 * Copy the latest completed measurement window levels (primary = mic 0).
 * Returns ESP_OK with valid=false when no window has been measured yet.
 * Field set matches sp_mic_get_channel(0) for rms_dbfs / window_seq /
 * peak / timing / transport / microphone_status.
 */
esp_err_t sp_mic_get_levels(sp_mic_levels_t *out);

/**
 * Copy the latest completed window for one live channel (Stage 1).
 * index must be < live microphone count; otherwise ESP_ERR_INVALID_ARG.
 * Both stereo channels share the same window_seq / measured_at_us window.
 * Does not change Observation / activity — use get_levels() for primary path.
 */
esp_err_t sp_mic_get_channel(uint8_t index, sp_mic_channel_t *out);

/**
 * Copy shared stream metadata for the latest completed window (Stage 1).
 * available_mic_count counts channels that delivered samples; it is not a
 * health verdict (open SD can still yield slot words / silence / noise).
 */
esp_err_t sp_mic_get_stream(sp_mic_stream_t *out);

#ifdef __cplusplus
}
#endif
