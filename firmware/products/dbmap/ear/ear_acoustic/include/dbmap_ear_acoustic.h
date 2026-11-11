#pragma once

/**
 * dBmap Ear product acoustic runtime (L0 / L1 / L2) on platform analysis PCM.
 *
 * Historical poll/get APIs keep dbmap_ear_mic_l0/l1/l2_* names for call-site
 * compatibility; Platform capture APIs are sp_mic_*.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dbmap_acoustic_l0.h"
#include "dbmap_acoustic_l1.h"
#include "dbmap_acoustic_l2.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Install the PCM-ready hook that inits L0 filter bank / L1 workspace.
 * Call before sp_mic_start.
 */
esp_err_t dbmap_ear_acoustic_register(void);

/**
 * L0 analysis CPU/memory counters (Phase B local validation).
 * analyze_* are wall times for dbmap_l0_analyze_window on-device.
 * pcm_* fields are filled from platform mic PCM status.
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
 * L1 runs in dbmap_ear_mic_l0_poll after L0 when DBMAP_EAR_OBS_L1 is enabled
 * and L2 is off. When L2 is on, these analyze_us_* fields are not updated
 * (combined fused time is reported via dbmap_ear_mic_l2_perf_t instead).
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

/**
 * Pre-L2 EAR-001 L1 baseline (µs) for derived L2 incremental estimates.
 * Not a direct measurement of the fused path.
 */
#ifndef DBMAP_L2_L1_BASELINE_US_LAST
#define DBMAP_L2_L1_BASELINE_US_LAST 40000u
#endif
#ifndef DBMAP_L2_L1_BASELINE_US_MAX
#define DBMAP_L2_L1_BASELINE_US_MAX 45000u
#endif

/**
 * L2 / fused spectral timing (when DBMAP_EAR_OBS_L2).
 * combined_* is direct wall time of dbmap_l2_analyze_window.
 * incremental_estimated_* = combined − EAR-001 L1 baseline (derived).
 * rms_* is optional direct stage time for raw-frame RMS only.
 */
typedef struct {
    bool enabled;
    uint32_t analyze_count;
    uint32_t combined_us_last;
    uint32_t combined_us_max;
    uint32_t incremental_estimated_us_last;
    uint32_t incremental_estimated_us_max;
    uint32_t rms_us_last;
    uint32_t rms_us_max;
    uint32_t baseline_l1_us_last;
    uint32_t baseline_l1_us_max;
} dbmap_ear_mic_l2_perf_t;

esp_err_t dbmap_ear_mic_get_l2_window(dbmap_l2_window_t *out);
esp_err_t dbmap_ear_mic_get_l2_perf(dbmap_ear_mic_l2_perf_t *out);

#ifdef __cplusplus
}
#endif
