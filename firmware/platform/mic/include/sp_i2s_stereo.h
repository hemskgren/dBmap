/**
 * Host-testable I2S stereo frame unpack for Ear multi-mic capture.
 *
 * ESP-IDF (ESP32-S3 STD RX, Philips, ws_pol=false) documents the receive
 * buffer for stereo as interleaved WS-low then WS-high words. Classic Philips
 * treats WS-low as left and WS-high as right. Per DIN pair this Ear maps:
 *   even slot words → left  → mic index (din*2 + 0)
 *   odd  slot words → right → mic index (din*2 + 1)
 * DIN0 → mic 0/1; DIN1 → mic 2/3.
 *
 * That mapping is a software contract derived from IDF docs + INMP441 L/R
 * strap (GND=left, VDD=right). It is NOT proof of physical board wiring until
 * hardware verification independently exercises each channel.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Words per sample period on one DIN: 1 (mono) or 2 (stereo L+R). */
uint32_t sp_i2s_slot_words_per_frame(uint8_t channel_count);

/**
 * How many channels a DIN carries for a given live microphone count.
 * din 0: 1 if live==1, else 2 when live>=2, else 0.
 * din 1: 0 if live<=2, 1 if live==3, 2 if live>=4.
 */
uint8_t sp_i2s_channels_on_din(uint8_t live_count, uint8_t din_index);

/** Absolute mic index for DIN + L/R slot (0=left, 1=right). */
uint8_t sp_i2s_mic_index_for_din_slot(uint8_t din_index, uint8_t slot_lr);

/**
 * How many complete stereo pairs fit in slot_word_count 32-bit words.
 * Odd trailing words are incomplete and are not counted as pairs.
 */
size_t sp_i2s_stereo_complete_pairs(size_t slot_word_count);

/** 1 if slot_word_count is odd (trailing incomplete frame), else 0. */
uint32_t sp_i2s_stereo_incomplete_trailing(size_t slot_word_count);

/**
 * Deterministic channel index for a stereo slot word position.
 * even → 0 (left/mic0), odd → 1 (right/mic1).
 */
uint8_t sp_i2s_stereo_channel_index(size_t slot_word_index);

typedef int32_t (*sp_i2s_extract_fn)(uint32_t slot_word);

/**
 * Unpack interleaved stereo slot words into left/right PCM.
 *
 * Only complete pairs are written. Returns number of pairs unpacked.
 * out_incomplete is set to 1 when slot_word_count is odd (trailing word dropped).
 * left_out/right_out may be NULL to count only.
 */
size_t sp_i2s_unpack_stereo_slots(
    const uint32_t *slots,
    size_t slot_word_count,
    sp_i2s_extract_fn extract,
    int32_t *left_out,
    int32_t *right_out,
    size_t max_pairs,
    uint32_t *out_incomplete
);

/**
 * Channel availability vs health (host-testable).
 * Delivering samples ≠ healthy microphone.
 */
typedef enum {
    SP_MIC_CH_NO_SAMPLES = 0,     /* not available this window */
    SP_MIC_CH_SILENT,             /* samples present but all-zero / digital silence */
    SP_MIC_CH_CLIPPING,           /* samples present with clip hits */
    SP_MIC_CH_ACTIVE,             /* samples present, not all-zero (may still be noise) */
} sp_mic_channel_presence_t;

/**
 * Classify presence from counts + peak floor (not mic structure heuristic).
 * clipped_samples > 0 with non-zero sample_count → CLIPPING (still "available").
 * all_zero when zero_sample_count == sample_count > 0 → SILENT.
 * peak_dbfs at/below DBMAP_AUDIO_SILENCE_DBFS → SILENT (sub-LSB energy / digital quiet).
 */
sp_mic_channel_presence_t sp_mic_channel_presence(
    uint32_t sample_count,
    uint32_t zero_sample_count,
    uint32_t clipped_samples,
    float peak_dbfs
);

const char *sp_mic_channel_presence_str(sp_mic_channel_presence_t p);

#ifdef __cplusplus
}
#endif
