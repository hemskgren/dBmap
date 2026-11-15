#pragma once

/**
 * dBmap Ear product acoustic runtime (L0 / L1 / L2) on platform analysis PCM.
 *
 * Uses Platform sp_audio_analyze_* for per-channel DSP (every active mic ×
 * every build-enabled layer, serial L1 workspace). Historical poll/get APIs
 * keep dbmap_ear_mic_l0/l1/l2_* names; mic0 aliases preserve single-mic
 * Observation behavior. DSP algorithms remain Platform-owned (sp_acoustic_l*).
 */

#include <stdbool.h>
#include <stdint.h>

#include "dbmap_acoustic_l0.h"
#include "dbmap_acoustic_l1.h"
#include "dbmap_acoustic_l2.h"
#include "sp_audio_analyze.h"
#include "sp_mic_pcm.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Install the PCM-ready hook that inits the Platform multi-channel analyze
 * context (L0 filter bank / L1 workspace). Call before sp_mic_start.
 */
esp_err_t dbmap_ear_acoustic_register(void);

/**
 * L0 analysis CPU/memory counters (Phase B local validation).
 * analyze_* are wall times for multi-channel analyze on-device.
 * pcm_* fields are filled from platform mic PCM status.
 */
typedef struct {
    bool enabled;
    bool pcm_ready;
    uint32_t pcm_cap_samples;
    uint32_t pcm_bytes_total; /* both double-buffers × retained channels */
    uint32_t analyze_count;
    uint32_t analyze_us_last;
    uint32_t analyze_us_max;
    /** Sum of wall-clock analyze µs for avg = sum/count (multi-mic pass). */
    uint64_t analyze_us_sum;
    /** Stages from last serial channel L0 call (not mic0-specific under multi-mic). */
    uint32_t stage_reset_us;
    uint32_t stage_sample_loop_us;
    uint32_t stage_post_us;
    uint32_t pending_overwrite; /* capture produced a window before prior was analyzed */
    uint32_t free_heap_after_init;
    uint32_t free_spiram_after_init;
    /** Active mic count for the last successful analyze pass. */
    uint8_t last_active_mic_count;
} dbmap_ear_mic_l0_perf_t;

/**
 * Analyze an already-taken Platform PCM job into per-channel L0(/L1/L2).
 * Used by Audio Engine device intake (sole PCM taker when AE enabled) and by
 * dbmap_ear_mic_l0_poll when AE is off. Does not call sp_mic_take_pcm_job.
 * Processes every active microphone with every build-enabled DSP layer.
 */
esp_err_t dbmap_ear_acoustic_analyze_pcm_job(const sp_mic_pcm_job_t *job);

/**
 * Run pending L0 analysis for the most recently captured PCM window.
 * Safe to call from the observation task; does not block I2S capture
 * (double-buffered SPIRAM PCM). No-op / ESP_ERR_NOT_SUPPORTED when L0 off.
 * When Audio Engine device intake is active, prefer ae_device_poll() which
 * takes PCM once and calls dbmap_ear_acoustic_analyze_pcm_job.
 */
esp_err_t dbmap_ear_mic_l0_poll(void);

/**
 * Copy the latest completed L0 analysis window for mic 0 (after l0_poll).
 * valid=false until the first successful analysis. Preserves single-mic API.
 */
esp_err_t dbmap_ear_mic_get_l0_window(dbmap_l0_window_t *out);

/**
 * Copy latest L0 window for channel_id (0..active-1).
 * ESP_ERR_INVALID_ARG when channel_id is inactive / out of range.
 */
esp_err_t dbmap_ear_mic_get_l0_window_channel(uint8_t channel_id, dbmap_l0_window_t *out);

/** Copy L0 performance counters (zeros / enabled=false when L0 off). */
esp_err_t dbmap_ear_mic_get_l0_perf(dbmap_ear_mic_l0_perf_t *out);

/**
 * L1 analysis timing counters (Step 3). Times are wall-clock µs for the
 * multi-channel analyze pass (serial L1 workspace reuse). When L2 is on,
 * these analyze_us_* fields are not updated (combined fused time via L2 perf).
 */
typedef struct {
    bool enabled;
    uint32_t analyze_count;
    uint32_t analyze_us_last;
    uint32_t analyze_us_max;
    uint64_t analyze_us_sum;
} dbmap_ear_mic_l1_perf_t;

/** Latest L1 window for mic 0. */
esp_err_t dbmap_ear_mic_get_l1_window(dbmap_l1_window_t *out);

/** Latest L1 window for channel_id. */
esp_err_t dbmap_ear_mic_get_l1_window_channel(uint8_t channel_id, dbmap_l1_window_t *out);

/** Copy L1 performance counters (enabled=false when L1 off). */
esp_err_t dbmap_ear_mic_get_l1_perf(dbmap_ear_mic_l1_perf_t *out);

#ifndef DBMAP_L2_L1_BASELINE_US_LAST
#define DBMAP_L2_L1_BASELINE_US_LAST 40000u
#endif
#ifndef DBMAP_L2_L1_BASELINE_US_MAX
#define DBMAP_L2_L1_BASELINE_US_MAX 45000u
#endif

/**
 * L2 / fused spectral timing (when DBMAP_EAR_OBS_L2).
 * combined_* is direct wall time of the multi-channel fused analyze pass.
 */
typedef struct {
    bool enabled;
    uint32_t analyze_count;
    uint32_t combined_us_last;
    uint32_t combined_us_max;
    uint64_t combined_us_sum;
    uint32_t incremental_estimated_us_last;
    uint32_t incremental_estimated_us_max;
    uint32_t rms_us_last;
    uint32_t rms_us_max;
    uint32_t baseline_l1_us_last;
    uint32_t baseline_l1_us_max;
} dbmap_ear_mic_l2_perf_t;

esp_err_t dbmap_ear_mic_get_l2_window(dbmap_l2_window_t *out);
esp_err_t dbmap_ear_mic_get_l2_window_channel(uint8_t channel_id, dbmap_l2_window_t *out);
esp_err_t dbmap_ear_mic_get_l2_perf(dbmap_ear_mic_l2_perf_t *out);

/**
 * Copy the latest multi-channel Platform analysis result set.
 * Identifies each channel by (window_seq, channel_id).
 */
esp_err_t dbmap_ear_acoustic_get_window_result(sp_audio_window_result_t *out);

/** Active mic count used by the last successful analyze (0 if none yet). */
uint8_t dbmap_ear_acoustic_active_mic_count(void);

#ifdef __cplusplus
}
#endif
