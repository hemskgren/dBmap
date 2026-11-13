/**
 * Audio Engine device intake (M5).
 *
 * Sole consumer of sp_mic_take_window_job when enabled. Runs acoustic analyze
 * (mic0 PCM for L0–L2), aligns Mic0/Mic1, evaluates stereo foundation state,
 * and publishes signatures to generic listeners. Does not own Candidate Engine.
 *
 * Temporary: polled from ear_obs (§0.9) — not a permanent Platform contract.
 */
#pragma once

#include "ae_engine.h"
#include "ae_stereo.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t pcm_jobs_taken;
    uint32_t analyze_ok;
    uint32_t analyze_fail;
    uint32_t sig_published;
    uint32_t mic0_aligned;
    uint32_t mic1_aligned;
    uint32_t pair_aligned;
    uint32_t mismatch_reject_pair;
    uint32_t no_job;
    uint32_t last_pcm_seq;
    int64_t last_pcm_end_us;
    uint32_t last_mismatch_flags;
    uint8_t last_quality;
    /** Retained for log compatibility; CE is Ear-owned (always 0 from AE). */
    uint8_t ce_candidates;
    uint32_t pcm_overwrite_events;
    uint32_t seq_gap_events;
    uint32_t gap_sigs_published;
    /** Last stereo foundation evaluation (M5). */
    uint8_t last_stereo_state;
    uint8_t last_pcm_mask;
    uint32_t last_pcm_n0;
    uint32_t last_pcm_n1;
    float last_mic0_rms_dbfs;
    float last_mic1_rms_dbfs;
    uint32_t stereo_ok;
    uint32_t stereo_mono_fallback;
    uint32_t stereo_misaligned;
    uint32_t stereo_degraded;
    uint32_t stereo_pcm_incomplete;
} ae_device_diag_t;

esp_err_t ae_device_start(void);
void ae_device_stop(void);
bool ae_device_active(void);
esp_err_t ae_device_poll(void);
void ae_device_get_diag(ae_device_diag_t *out);
ae_engine_t *ae_device_engine(void);

#ifdef __cplusplus
}
#endif
