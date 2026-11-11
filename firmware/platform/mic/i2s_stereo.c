#include "sp_i2s_stereo.h"

#include "sp_audio_metrics.h"

#include <stddef.h>

uint32_t sp_i2s_slot_words_per_frame(uint8_t channel_count)
{
    return channel_count >= 2u ? 2u : 1u;
}

uint8_t sp_i2s_channels_on_din(uint8_t live_count, uint8_t din_index)
{
    if (din_index == 0u) {
        if (live_count < 1u) {
            return 0u;
        }
        return live_count == 1u ? 1u : 2u;
    }
    if (din_index == 1u) {
        if (live_count <= 2u) {
            return 0u;
        }
        if (live_count == 3u) {
            return 1u;
        }
        return 2u;
    }
    return 0u;
}

uint8_t sp_i2s_mic_index_for_din_slot(uint8_t din_index, uint8_t slot_lr)
{
    return (uint8_t)((din_index * 2u) + (slot_lr & 1u));
}

size_t sp_i2s_stereo_complete_pairs(size_t slot_word_count)
{
    return slot_word_count / 2u;
}

uint32_t sp_i2s_stereo_incomplete_trailing(size_t slot_word_count)
{
    return (slot_word_count & 1u) != 0u ? 1u : 0u;
}

uint8_t sp_i2s_stereo_channel_index(size_t slot_word_index)
{
    return (uint8_t)(slot_word_index & 1u);
}

size_t sp_i2s_unpack_stereo_slots(
    const uint32_t *slots,
    size_t slot_word_count,
    sp_i2s_extract_fn extract,
    int32_t *left_out,
    int32_t *right_out,
    size_t max_pairs,
    uint32_t *out_incomplete
)
{
    if (out_incomplete != NULL) {
        *out_incomplete = sp_i2s_stereo_incomplete_trailing(slot_word_count);
    }
    if (slots == NULL || extract == NULL) {
        return 0;
    }

    size_t pairs = sp_i2s_stereo_complete_pairs(slot_word_count);
    if (pairs > max_pairs) {
        pairs = max_pairs;
    }

    for (size_t i = 0; i < pairs; ++i) {
        const uint32_t left_slot = slots[i * 2u];
        const uint32_t right_slot = slots[i * 2u + 1u];
        const int32_t left = extract(left_slot);
        const int32_t right = extract(right_slot);
        if (left_out != NULL) {
            left_out[i] = left;
        }
        if (right_out != NULL) {
            right_out[i] = right;
        }
    }
    return pairs;
}

sp_mic_channel_presence_t sp_mic_channel_presence(
    uint32_t sample_count,
    uint32_t zero_sample_count,
    uint32_t clipped_samples,
    float peak_dbfs
)
{
    if (sample_count == 0u) {
        return SP_MIC_CH_NO_SAMPLES;
    }
    if (zero_sample_count >= sample_count) {
        return SP_MIC_CH_SILENT;
    }
    /* Align with classify peak_silence: sub-LSB / clamped −120 is not "active". */
    if (!(peak_dbfs > DBMAP_AUDIO_SILENCE_DBFS)) {
        return SP_MIC_CH_SILENT;
    }
    if (clipped_samples > 0u) {
        return SP_MIC_CH_CLIPPING;
    }
    return SP_MIC_CH_ACTIVE;
}

const char *sp_mic_channel_presence_str(sp_mic_channel_presence_t p)
{
    switch (p) {
    case SP_MIC_CH_NO_SAMPLES:
        return "no_samples";
    case SP_MIC_CH_SILENT:
        return "silent";
    case SP_MIC_CH_CLIPPING:
        return "clipping";
    case SP_MIC_CH_ACTIVE:
        return "active";
    default:
        return "no_samples";
    }
}
