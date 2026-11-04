/**
 * Installation-level microphone count (NVS audio/mic_count).
 * Independent of config_version / calibration_version / classifier_version.
 * Host-testable pure helpers.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBMAP_MIC_COUNT_MIN 1u
#define DBMAP_MIC_COUNT_MAX 2u
#define DBMAP_MIC_COUNT_DEFAULT 1u

/** Diagnostic status for reporting (not a Desired/config_version field). */
typedef enum {
    DBMAP_MIC_COUNT_STATUS_OK = 0,           /* stored value valid */
    DBMAP_MIC_COUNT_STATUS_DEFAULTED,        /* key missing → default */
    DBMAP_MIC_COUNT_STATUS_INVALID_FALLBACK, /* corrupt/unsupported → default */
} dbmap_mic_count_status_t;

const char *dbmap_mic_count_status_str(dbmap_mic_count_status_t status);

/** True if count is a supported installation value (1 or 2). */
bool dbmap_mic_count_supported(uint8_t count);

/**
 * Normalize a raw NVS byte into an effective count + status.
 * Missing keys should call with found=false (raw ignored).
 */
void dbmap_mic_count_from_nvs(
    bool found,
    uint8_t raw,
    uint8_t *out_effective,
    dbmap_mic_count_status_t *out_status
);

/**
 * Validate a USB/JSON integer candidate.
 * Returns true only for exact integers 1 or 2 (caller rejects non-integers).
 */
bool dbmap_mic_count_from_json_int(int value, uint8_t *out_effective);

#ifdef __cplusplus
}
#endif
