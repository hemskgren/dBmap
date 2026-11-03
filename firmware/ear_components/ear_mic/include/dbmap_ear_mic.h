#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "dbmap_acoustic_l0.h"
#include "dbmap_acoustic_l1.h"
#include "dbmap_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One microphone model per Ear build. Additional models can be added later;
 * Kconfig selects exactly one active model for a given firmware image.
 */
typedef enum {
    DBMAP_EAR_MIC_MODEL_INMP441 = 1,
} dbmap_ear_mic_model_t;

typedef struct {
    dbmap_ear_mic_model_t model;
    int bclk_gpio;
    int ws_gpio;
    int din_gpio;
    int mclk_gpio; /* -1 if unused */
    int sample_rate_hz;
} dbmap_ear_mic_config_t;

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
} dbmap_ear_mic_levels_t;

/**
 * Start the bounded I2S acquisition + telemetry task.
 * Registers a hello/health enricher that publishes microphone capabilities.
 * cfg must remain valid for the lifetime of the task (app_main static config).
 */
esp_err_t dbmap_ear_mic_start(const dbmap_effective_t *cfg, const dbmap_ear_mic_config_t *mic_cfg);

/** Active model id string, e.g. "INMP441". */
const char *dbmap_ear_mic_model_id(void);

/** Latest subsystem status string: ok | degraded | error | unavailable. */
const char *dbmap_ear_mic_subsystem_status(void);

/**
 * Copy the latest completed measurement window levels.
 * Returns ESP_OK with valid=false when no window has been measured yet.
 */
esp_err_t dbmap_ear_mic_get_levels(dbmap_ear_mic_levels_t *out);

/**
 * L0 analysis CPU/memory counters (Phase B local validation).
 * analyze_* are wall times for dbmap_l0_analyze_window on-device.
 */
typedef struct {
    bool enabled;
    bool pcm_ready;
    uint32_t pcm_cap_samples;
    uint32_t pcm_bytes_total; /* both double-buffers */
    uint32_t analyze_count;
    uint32_t analyze_us_last;
    uint32_t analyze_us_max;
    /** Stages inside dbmap_l0_analyze_window (last call). */
    uint32_t stage_reset_us;
    uint32_t stage_sample_loop_us;
    uint32_t stage_post_us;
    uint32_t pending_overwrite; /* capture produced a window before prior was analyzed */
    uint32_t free_heap_after_init;
    uint32_t free_spiram_after_init;
} dbmap_ear_mic_l0_perf_t;

/**
 * Run pending L0 analysis for the most recently captured PCM window.
 * Safe to call from the observation task; does not block I2S capture
 * (double-buffered SPIRAM PCM). No-op / ESP_ERR_NOT_SUPPORTED when L0 off.
 */
esp_err_t dbmap_ear_mic_l0_poll(void);

/**
 * Copy the latest completed L0 analysis window (after l0_poll).
 * valid=false until the first successful analysis.
 */
esp_err_t dbmap_ear_mic_get_l0_window(dbmap_l0_window_t *out);

/** Copy L0 performance counters (zeros / enabled=false when L0 off). */
esp_err_t dbmap_ear_mic_get_l0_perf(dbmap_ear_mic_l0_perf_t *out);

/**
 * L1 analysis timing counters (Step 3). Times are wall-clock µs for
 * dbmap_l1_analyze_window only (esp_timer), not task CPU time.
 * L1 runs in dbmap_ear_mic_l0_poll after L0 when DBMAP_EAR_OBS_L1 is enabled.
 */
typedef struct {
    bool enabled;
    uint32_t analyze_count;
    uint32_t analyze_us_last;
    uint32_t analyze_us_max;
} dbmap_ear_mic_l1_perf_t;

/**
 * Copy the latest completed L1 analysis window (after l0_poll with L1 on).
 * valid=false until the first successful analysis; ESP_ERR_NOT_SUPPORTED if L1 off.
 */
esp_err_t dbmap_ear_mic_get_l1_window(dbmap_l1_window_t *out);

/** Copy L1 performance counters (enabled=false when L1 off). */
esp_err_t dbmap_ear_mic_get_l1_perf(dbmap_ear_mic_l1_perf_t *out);

#ifdef __cplusplus
}
#endif
