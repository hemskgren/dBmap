#pragma once

#include "cJSON.h"
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
 * Start the bounded I2S acquisition + telemetry task.
 * Registers a hello/health enricher that publishes microphone capabilities.
 * cfg must remain valid for the lifetime of the task (app_main static config).
 */
esp_err_t dbmap_ear_mic_start(const dbmap_effective_t *cfg, const dbmap_ear_mic_config_t *mic_cfg);

/** Active model id string, e.g. "INMP441". */
const char *dbmap_ear_mic_model_id(void);

/** Latest subsystem status string: ok | degraded | error | unavailable. */
const char *dbmap_ear_mic_subsystem_status(void);

#ifdef __cplusplus
}
#endif
