#include "dbmap_ear_mic_channel_api.h"

#include "dbmap_audio_metrics.h"

#include <string.h>

static void copy_status(char *dst, size_t dst_sz, const char *src, const char *fallback)
{
    if (dst == NULL || dst_sz == 0) {
        return;
    }
    memset(dst, 0, dst_sz);
    const char *use = (src != NULL && src[0] != '\0') ? src : fallback;
    if (use == NULL) {
        use = "unavailable";
    }
    strncpy(dst, use, dst_sz - 1u);
}

int dbmap_ear_mic_fill_channel(
    uint8_t index,
    uint8_t live_mic_count,
    const dbmap_ear_mic_window_view_t *view,
    uint32_t sample_rate_hz,
    dbmap_ear_mic_channel_t *out
)
{
    if (out == NULL || view == NULL || live_mic_count < 1u || index >= live_mic_count) {
        return DBMAP_EAR_MIC_API_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->index = index;
    strncpy(out->slot, index == 0u ? "left" : "right", sizeof(out->slot) - 1u);
    out->valid = (view->measured_at_us > 0);
    out->rms_dbfs = view->rms_dbfs;
    out->peak_dbfs = view->peak_dbfs;
    out->sample_count = view->sample_count;
    out->expected_sample_count =
        dbmap_audio_expected_samples(sample_rate_hz, view->window_ms);
    out->window_seq = view->window_seq;
    out->window_ms = view->window_ms;
    out->measured_at_us = view->measured_at_us;
    out->ms_since_i2s_enable = view->ms_since_i2s_enable;
    copy_status(
        out->transport_status,
        sizeof(out->transport_status),
        view->transport_status,
        "unavailable"
    );
    copy_status(
        out->microphone_status,
        sizeof(out->microphone_status),
        view->microphone_status,
        "unavailable"
    );
    copy_status(
        out->channel_presence,
        sizeof(out->channel_presence),
        view->channel_presence,
        "no_samples"
    );
    if (view->classification_reason[0] != '\0') {
        strncpy(
            out->classification_reason,
            view->classification_reason,
            sizeof(out->classification_reason) - 1u
        );
    }
    out->zero_sample_count = view->zero_sample_count;
    out->all_zero_window = view->all_zero_window;
    out->clipped_samples = view->clipped_samples;
    out->read_errors = view->read_errors;
    out->timeout_reads = view->timeout_reads;
    out->empty_reads = view->empty_reads;
    out->missing_data = view->missing_data;
    out->counters_capped = view->counters_capped;
    out->raw_min = view->raw_min;
    out->raw_max = view->raw_max;
    out->full_scale_sample_count = view->full_scale_sample_count;
    out->unique_sample_count = view->unique_sample_count;
    out->consecutive_identical_samples = view->consecutive_identical_samples;
    out->unique_capped = view->unique_capped;
    out->read_calls = view->read_calls;
    out->bytes_received = view->bytes_received;
    out->complete_frames = view->complete_frames;
    out->partial_reads = view->partial_reads;
    out->raw_slot_min = view->raw_slot_min;
    out->raw_slot_max = view->raw_slot_max;
    out->slot_zero_count = view->slot_zero_count;
    out->slot_msb_set_count = view->slot_msb_set_count;
    return DBMAP_EAR_MIC_API_OK;
}

int dbmap_ear_mic_fill_primary_levels(
    const dbmap_ear_mic_window_view_t *view,
    dbmap_ear_mic_primary_levels_t *out
)
{
    if (out == NULL || view == NULL) {
        return DBMAP_EAR_MIC_API_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    if (view->measured_at_us > 0) {
        out->valid = true;
        out->rms_dbfs = view->rms_dbfs;
        out->peak_dbfs = view->peak_dbfs;
        out->window_ms = view->window_ms;
        out->measured_at_us = view->measured_at_us;
        out->window_seq = view->window_seq;
        out->ms_since_i2s_enable = view->ms_since_i2s_enable;
        copy_status(
            out->transport_status,
            sizeof(out->transport_status),
            view->transport_status,
            "unavailable"
        );
        copy_status(
            out->microphone_status,
            sizeof(out->microphone_status),
            view->microphone_status,
            "unavailable"
        );
    }
    return DBMAP_EAR_MIC_API_OK;
}

int dbmap_ear_mic_fill_stream(
    uint8_t live_mic_count,
    uint8_t configured_mic_count,
    const dbmap_ear_mic_window_view_t *views,
    const char *slot_mode,
    uint32_t incomplete_stereo_frames,
    dbmap_ear_mic_stream_t *out
)
{
    if (out == NULL || views == NULL || live_mic_count < 1u) {
        return DBMAP_EAR_MIC_API_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->live_mic_count = live_mic_count;
    out->configured_mic_count = configured_mic_count;
    out->incomplete_stereo_frames = incomplete_stereo_frames;
    out->window_seq = views[0].window_seq;
    if (slot_mode != NULL && slot_mode[0] != '\0') {
        strncpy(out->slot_mode, slot_mode, sizeof(out->slot_mode) - 1u);
    }

    uint8_t available = 0;
    for (uint8_t i = 0; i < live_mic_count; ++i) {
        if (views[i].measured_at_us > 0
            && views[i].missing_data == 0
            && views[i].sample_count > 0) {
            available++;
        }
    }
    out->available_mic_count = available;
    return DBMAP_EAR_MIC_API_OK;
}
