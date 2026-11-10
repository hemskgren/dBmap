#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dbmap_acoustic_l0.h"
#include "dbmap_acoustic_l1.h"
#include "dbmap_acoustic_l2.h"
#include "dbmap_observation_payload.h"
#include "dbmap_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Ear Observation publisher + RMS activity detection + Stage 1 episode registry.
 *
 * MQTT Observations use protocol_version 1 hierarchical envelope only
 * (identity/context/acoustic/spatial). L0 measurements are required.
 * Stable observation_id per episode; global sequence_number per report.
 */

/**
 * Register the OTA acoustic-close callback with node_common.
 *
 * Must be called from Ear app_main after MQTT start and before any OTA-capable
 * Desired operation (including dbmap_desired_boot_check). Returns an explicit
 * error if registration cannot be installed; callers must not proceed to OTA
 * paths as if it succeeded. Safe before dbmap_ear_observation_start (close is a
 * no-op until an episode exists). Close ends observation episodes only; it does
 * not stop I2S/microphone capture (Design B may already have mic running).
 */
esp_err_t dbmap_ear_observation_register_ota_close(void);

/**
 * Start Observation support: load/reserve sequence block, register capabilities,
 * start the detection/publish task (when RMS activity is enabled).
 * Call after dbmap_ear_mic_start so health enrichers chain correctly.
 * cfg must remain valid for the process lifetime.
 */
esp_err_t dbmap_ear_observation_start(const dbmap_effective_t *cfg);

/**
 * Publish one hierarchical Observation report (QoS 1, not retained).
 *
 * observation_id must be the stable episode UUID (caller-owned).
 * Allocates a global sequence number only when MQTT is connected.
 * Once allocate() returns, the number is consumed even if publish fails.
 *
 * emission_kind: "start" | "periodic" | "end" | "complete"
 * termination_reason: required for end/complete; NULL for start/periodic.
 * measurements: required (may be empty-interval with null metrics).
 * signature: NULL → acoustic.signature JSON null (L1 disabled); non-NULL →
 *   schema=l1 object (may be empty).
 * dynamics: NULL → acoustic.dynamics JSON null (L2 publish off / omitted);
 *   non-NULL → schema=l2 object (may be empty or Option A overflow).
 * has_signal_level_dbfs: false → JSON null signal level.
 */
esp_err_t dbmap_ear_observation_publish(
    const dbmap_effective_t *cfg,
    const char *observation_id,
    bool has_signal_level_dbfs,
    double signal_level_dbfs,
    double interval_duration_ms,
    double episode_duration_ms,
    int64_t capture_timestamp_monotonic_us,
    int64_t interval_start_monotonic_us,
    const char *emission_kind,
    const char *termination_reason,
    const dbmap_l0_interval_t *measurements,
    const dbmap_l1_interval_t *signature,
    const dbmap_l2_interval_t *dynamics
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
