#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * RAM-only device diagnostics (D1).
 *
 * Defaults come from Kconfig and can later be overridden by Desired
 * `payload.diag` when a configuration sync bumps `config_version`.
 * Counters are not written to NVS; they clear on reboot.
 *
 * Counter meanings:
 * - wifi_disconnects: WIFI_EVENT_STA_DISCONNECTED observations
 * - mqtt_disconnects: transitions from MQTT connected → disconnected
 * - mqtt_reconnects: successful MQTT CONNECT after a prior disconnect
 * - mqtt_connect_attempts: MQTT connect/reconnect starts (incl. first start)
 * - mqtt_connect_failures: connect/reconnect endings that did not reach connected
 * - desired_failures: Desired HTTPS checks that failed (transport or HTTP)
 */
typedef struct {
    bool enabled;
    int rssi_degraded_dbm;
    uint16_t counter_saturate;
} sp_diag_config_t;

void sp_diag_init(void);
void sp_diag_config_defaults(sp_diag_config_t *out);

/** Merge optional Desired `payload.diag` object; unknown keys ignored. */
void sp_diag_apply_desired_payload(const cJSON *payload);

const sp_diag_config_t *sp_diag_config(void);

void sp_diag_note_wifi_disconnect(void);
void sp_diag_note_mqtt_disconnect(void);
void sp_diag_note_mqtt_reconnect(void);
void sp_diag_note_mqtt_connect_attempt(void);
void sp_diag_note_mqtt_connect_failure(void);
void sp_diag_note_desired_failure(void);

/** Observation MQTT publish diagnostics (RAM-only; omit-when-zero on wire). */
void sp_diag_note_obs_start_rejected(void);
void sp_diag_note_obs_periodic_rejected(void);
void sp_diag_note_obs_end_rejected(void);
void sp_diag_note_obs_complete_rejected(void);
void sp_diag_note_obs_skipped_disconnected(void);

/**
 * Attach a compact `diag` object when enabled and there is something to report.
 * May include anomaly-only nested `mem` metrics (see sp_diag_memory.h).
 * After a successful MQTT publish of health/keepalive, call
 * sp_diag_clear_oneshots() so reset reason is sent at most once per boot and
 * a memory anomaly fingerprint is marked published.
 */
void sp_diag_attach_json(cJSON *state_object);
void sp_diag_clear_oneshots(void);

/**
 * Optional named FreeRTOS tasks for stack HWM in sp_diag_log_resources.
 * Call once after xTaskCreate (e.g. "sp_mic", "ear_obs"). Replaces prior
 * registration for the same name. Pass NULL handle to clear a slot.
 * Bounded to SP_DIAG_TRACKED_TASKS; extra registrations are ignored.
 */
#ifndef SP_DIAG_TRACKED_TASKS
#define SP_DIAG_TRACKED_TASKS 4u
#endif

void sp_diag_register_task(const char *name, void *task_handle);

/**
 * Shared UART resource snapshot (CPU / heap / SPIRAM / stack HWM).
 * Tag is always `sp`. ctx names the caller (e.g. "ear_obs") without owning the format.
 * Keep off the audio hot path — call from periodic diag only.
 *
 * Logs (bytes unless noted):
 * - cpu_mhz (esp_clk_cpu_freq), configured_mhz (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ),
 *   tick_hz (CONFIG_FREERTOS_HZ compile-time tick rate)
 * - free_heap (esp_get_free_heap_size — with SPIRAM_USE_MALLOC this is combined)
 * - min_free_heap (esp_get_minimum_free_heap_size — combined)
 * - free_internal / min_free_internal / largest_internal (MALLOC_CAP_INTERNAL)
 * - free_spiram / min_free_spiram / largest_spiram (MALLOC_CAP_SPIRAM)
 * - stack_hwm_bytes for current task, then each registered task
 */
void sp_diag_log_resources(const char *ctx);

#ifdef __cplusplus
}
#endif
