#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Host-testable audio window math. Levels are digital full-scale (dBFS), not dB SPL.
 * full_scale_bits is the signed peak magnitude bit width (e.g. 24 for INMP441 useful bits).
 */

float dbmap_audio_rms_dbfs(const int32_t *samples, size_t count, int full_scale_bits);
float dbmap_audio_peak_dbfs(const int32_t *samples, size_t count, int full_scale_bits);
uint32_t dbmap_audio_count_clipped(const int32_t *samples, size_t count, int full_scale_bits);

/**
 * INMP441: assume 24-bit PCM left-justified in a 32-bit I2S slot word.
 * Physical bit alignment is unverified until scoped on hardware.
 */
int32_t dbmap_inmp441_extract_sample(uint32_t slot_word);

#ifdef __cplusplus
}
#endif
