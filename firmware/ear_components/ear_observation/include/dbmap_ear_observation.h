#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dbmap_observation_payload.h"
#include "dbmap_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Ear Observation publisher + Phase 2 RMS activity detection.
 *
 * Protocol-v1 Observations use classification=null and bearing=null.
 * Sequence numbers use NVS block reservation (see observation_seq / README).
 */

/**
 * Start Observation support: load/reserve sequence block, register capabilities,
 * start the detection/publish task (when RMS activity is enabled).
 * Call after dbmap_ear_mic_start so health enrichers chain correctly.
 * cfg must remain valid for the process lifetime.
 */
esp_err_t dbmap_ear_observation_start(const dbmap_effective_t *cfg);

/**
 * Publish one Observation on cfg->topics.observation (QoS 1, not retained).
 *
 * Allocates a sequence number only when MQTT is connected (block-reserve as
 * needed). Once allocate() returns a number it is consumed — even if the MQTT
 * publish fails afterward. No offline queue: if MQTT is down, returns
 * ESP_ERR_INVALID_STATE without allocating.
 */
esp_err_t dbmap_ear_observation_publish(
    const dbmap_effective_t *cfg,
    double signal_level_dbfs,
    double duration_ms,
    int64_t capture_timestamp_monotonic_us,
    int64_t interval_start_monotonic_us
);

/**
 * Phase 1 controlled test trigger (optional Kconfig).
 * Prefer the RMS activity task for Phase 2. Safe to call from the main loop.
 */
void dbmap_ear_observation_poll_test_trigger(const dbmap_effective_t *cfg);

/** Next sequence number that will be assigned (diagnostics/tests). */
uint32_t dbmap_ear_observation_next_sequence(void);

/** Exclusive durable sequence limit last written to NVS (diagnostics/tests). */
uint32_t dbmap_ear_observation_seq_limit(void);

#ifdef __cplusplus
}
#endif
