/**
 * Stage 1 stereo channel/stream types and host-testable fillers.
 *
 * On device, ear_mic copies the latest completed window into a view under the
 * mic lock, then calls these helpers. No new measurement or classification.
 *
 * Timing references (all channels in one stereo window share these):
 * - window_seq: 1-based count of completed measure windows since I2S enable
 * - measured_at_us: esp_timer monotonic time at window finalize
 * - ms_since_i2s_enable: wall ms from i2s_channel_enable to window finalize
 * - window_ms: configured analysis window length
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBMAP_EAR_MIC_API_OK 0
#define DBMAP_EAR_MIC_API_ERR_INVALID_ARG 1

/**
 * Per-channel snapshot for diagnostics and future consumers.
 * Does not replace get_levels() for Observation / activity (primary = index 0).
 */
typedef struct {
    uint8_t index;
    char slot[8]; /* "left" | "right" — software contract, not HW proof */
    bool valid;   /* true when measured_at_us > 0 for this channel */
    float rms_dbfs;
    float peak_dbfs; /* same window as rms_dbfs; digital FS, not SPL */
    uint32_t sample_count;
    uint32_t expected_sample_count; /* rate_hz * window_ms / 1000 */
    uint32_t window_seq;
    uint32_t window_ms;
    int64_t measured_at_us;
    uint32_t ms_since_i2s_enable;
    char transport_status[16];
    char microphone_status[24];
    char channel_presence[16]; /* no_samples|silent|clipping|active */
    char classification_reason[48];
    uint32_t zero_sample_count;
    uint8_t all_zero_window;
    uint32_t clipped_samples;
    uint32_t read_errors;
    uint32_t timeout_reads;
    uint32_t empty_reads;
    uint32_t missing_data;
    uint8_t counters_capped;
    int32_t raw_min;
    int32_t raw_max;
    uint32_t full_scale_sample_count;
    uint32_t unique_sample_count;
    uint32_t consecutive_identical_samples;
    uint8_t unique_capped;
    uint32_t read_calls;
    uint32_t bytes_received;
    uint32_t complete_frames;
    uint32_t partial_reads;
    uint32_t raw_slot_min;
    uint32_t raw_slot_max;
    uint32_t slot_zero_count;
    uint32_t slot_msb_set_count;
} dbmap_ear_mic_channel_t;

/**
 * Shared stereo/mono stream metadata for one completed window.
 * window_seq is the common sequence for all live channels.
 *
 * available_mic_count: channels with samples this window (sample_count > 0 and
 * not missing_data). Not the same as "healthy" — an open SD line can still
 * deliver slot words (silence/noise) and count as available.
 */
typedef struct {
    uint8_t live_mic_count;
    uint8_t configured_mic_count;
    uint8_t available_mic_count;
    char slot_mode[16]; /* "mono_left" | "stereo_lr" */
    uint32_t incomplete_stereo_frames;
    uint32_t window_seq;
} dbmap_ear_mic_stream_t;

/**
 * Primary-path levels projection (same fields as dbmap_ear_mic_levels_t).
 * Host tests compare this to channel 0 without linking ear_mic.c.
 */
typedef struct {
    bool valid;
    float rms_dbfs;
    float peak_dbfs;
    uint32_t window_ms;
    int64_t measured_at_us;
    uint32_t window_seq;
    uint32_t ms_since_i2s_enable;
    char transport_status[16];
    char microphone_status[24];
} dbmap_ear_mic_primary_levels_t;

/** Completed-window view mirrored from ear_mic's internal snapshot. */
typedef struct {
    float rms_dbfs;
    float peak_dbfs;
    uint32_t sample_count;
    uint32_t window_ms;
    uint32_t window_seq;
    uint32_t ms_since_i2s_enable;
    int64_t measured_at_us;
    uint32_t zero_sample_count;
    uint8_t all_zero_window;
    uint32_t clipped_samples;
    uint32_t read_errors;
    uint32_t timeout_reads;
    uint32_t empty_reads;
    uint32_t missing_data;
    uint8_t counters_capped;
    int32_t raw_min;
    int32_t raw_max;
    uint32_t full_scale_sample_count;
    uint32_t unique_sample_count;
    uint32_t consecutive_identical_samples;
    uint8_t unique_capped;
    uint32_t read_calls;
    uint32_t bytes_received;
    uint32_t complete_frames;
    uint32_t partial_reads;
    uint32_t raw_slot_min;
    uint32_t raw_slot_max;
    uint32_t slot_zero_count;
    uint32_t slot_msb_set_count;
    char channel_presence[16];
    char transport_status[16];
    char microphone_status[24];
    char classification_reason[48];
} dbmap_ear_mic_window_view_t;

/**
 * Fill public channel struct from a completed-window view.
 * @return DBMAP_EAR_MIC_API_OK, or DBMAP_EAR_MIC_API_ERR_INVALID_ARG
 */
int dbmap_ear_mic_fill_channel(
    uint8_t index,
    uint8_t live_mic_count,
    const dbmap_ear_mic_window_view_t *view,
    uint32_t sample_rate_hz,
    dbmap_ear_mic_channel_t *out
);

/**
 * Fill primary levels (get_levels field set) from mic0's window view.
 * valid is true only when measured_at_us > 0.
 */
int dbmap_ear_mic_fill_primary_levels(
    const dbmap_ear_mic_window_view_t *view,
    dbmap_ear_mic_primary_levels_t *out
);

/**
 * Fill stream metadata. views has live_mic_count entries sharing window_seq.
 */
int dbmap_ear_mic_fill_stream(
    uint8_t live_mic_count,
    uint8_t configured_mic_count,
    const dbmap_ear_mic_window_view_t *views,
    const char *slot_mode,
    uint32_t incomplete_stereo_frames,
    dbmap_ear_mic_stream_t *out
);

#ifdef __cplusplus
}
#endif
