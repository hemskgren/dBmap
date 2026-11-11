/**
 * Installation-level microphone count (NVS audio/mic_count).
 * Independent of config_version / calibration_version / classifier_version.
 * Host-testable pure helpers.
 *
 * Software installation values: 1..SP_MIC_COUNT_MAX (4).
 * Physical I2S acquisition: up to SP_MIC_ACQUIRE_MAX (4) via dual STD RX
 * (I2S0 DIN0 master + I2S1 DIN1 slave sharing BCLK/WS). See
 * docs/device-wiring-and-pinout.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SP_MIC_COUNT_MIN 1u
#define SP_MIC_COUNT_MAX 4u
#define SP_MIC_COUNT_DEFAULT 1u

/**
 * Max channels this firmware acquires on I2S (DIN0 + DIN1 Philips pairs).
 * Configured count above this is clamped (should not happen while MAX==4).
 */
#define SP_MIC_ACQUIRE_MAX 4u

/** Diagnostic status for reporting (not a Desired/config_version field). */
typedef enum {
    SP_MIC_COUNT_STATUS_OK = 0,           /* stored value valid */
    SP_MIC_COUNT_STATUS_DEFAULTED,        /* key missing → default */
    SP_MIC_COUNT_STATUS_INVALID_FALLBACK, /* corrupt/unsupported → default */
} sp_mic_count_status_t;

const char *sp_mic_count_status_str(sp_mic_count_status_t status);

/** True if count is a supported installation value (1..SP_MIC_COUNT_MAX). */
bool sp_mic_count_supported(uint8_t count);

/**
 * Channels to open on I2S for a configured installation count.
 * Never invents channels beyond SP_MIC_ACQUIRE_MAX.
 */
uint8_t sp_mic_acquire_count(uint8_t configured_count);

/**
 * Normalize a raw NVS byte into an effective count + status.
 * Missing keys should call with found=false (raw ignored).
 */
void sp_mic_count_from_nvs(
    bool found,
    uint8_t raw,
    uint8_t *out_effective,
    sp_mic_count_status_t *out_status
);

/**
 * Validate a USB/JSON integer candidate.
 * Returns true only for exact integers in SP_MIC_COUNT_MIN..MAX.
 */
bool sp_mic_count_from_json_int(int value, uint8_t *out_effective);

#ifdef __cplusplus
}
#endif
