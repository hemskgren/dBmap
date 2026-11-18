/**
 * Bounded AE signature CSV capture for AS-001 calibration (bench / overlay).
 *
 * Compile-time gate: CONFIG_DBMAP_EAR_AE_SIG_CAPTURE (default off).
 * Observational only: does not create candidates, promote, reserve, bind, or MQTT.
 */
#pragma once

#include "ae_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Stable CSV line prefix for host grep/extract. */
#define AE_SIG_CSV_PREFIX "AE_SIG_CSV"

/** Default capacity: AS-001 is 32 windows; leave margin for settle + recede. */
#ifndef AE_SIG_CAPTURE_CAPACITY
#define AE_SIG_CAPTURE_CAPACITY 48u
#endif

/** Skip this many published signatures after start before arming CSV rows. */
#ifndef AE_SIG_CAPTURE_SETTLE_N
#define AE_SIG_CAPTURE_SETTLE_N 2u
#endif

/**
 * CSV header (no prefix). Field order is part of the capture contract.
 * Host tooling and tests must keep this in sync with ae_sig_capture_format_csv.
 */
#define AE_SIG_CSV_HEADER                                                      \
    "session_id,window_seq,window_end_mono_us,window_ms,sample_rate_hz,"       \
    "channel,quality,flags,clipped,"                                           \
    "rms_dbfs,peak_dbfs,crest_db,"                                             \
    "band0_dbfs,band1_dbfs,band2_dbfs,band3_dbfs,"                             \
    "band0_rel,band1_rel,band2_rel,band3_rel,"                                 \
    "hl_db,hl_valid,spectral_valid,centroid_hz,bandwidth_hz,dominant_hz,"      \
    "flatness,as001_matched,as001_rej_elig,as001_skip_spec,as001_profile_id,"  \
    "as001_dist"

typedef enum {
    AE_SIG_CAPTURE_DISABLED = 0,
    AE_SIG_CAPTURE_SETTLING = 1,
    AE_SIG_CAPTURE_ACTIVE = 2,
    AE_SIG_CAPTURE_COMPLETE = 3,
} ae_sig_capture_state_t;

typedef struct {
    uint32_t window_seq;
    int64_t window_end_mono_us;
    uint32_t window_ms;
    uint8_t channel; /* AE feature channel: 0 = L0 / left */
    uint8_t quality;
    uint8_t flags;
    uint8_t clipped;
    float rms_dbfs;
    float peak_dbfs;
    float crest_db;
    float band_dbfs[AE_BAND_COUNT];
    float band_rel[AE_BAND_COUNT];
    float hl_db;
    uint8_t hl_valid;
    uint8_t spectral_valid;
    float centroid_hz;
    float bandwidth_hz;
    float dominant_hz;
    float flatness;
    uint8_t as001_matched;
    uint8_t as001_rej_elig;
    uint8_t as001_skip_spec;
    uint16_t as001_profile_id;
    float as001_dist;
} ae_sig_capture_rec_t;

typedef struct {
    ae_sig_capture_state_t state;
    uint32_t session_id;
    uint32_t settle_remaining;
    uint32_t stored;
    uint32_t overflow;
    uint32_t drained;
    bool started_logged;
    bool ended_logged;
} ae_sig_capture_diag_t;

/** True when compiled with CONFIG_DBMAP_EAR_AE_SIG_CAPTURE. */
bool ae_sig_capture_compiled(void);

/** Reset buffer and arm settle → active → complete lifecycle. No-op if disabled. */
void ae_sig_capture_reset(uint32_t session_id);

/** Hot-path safe: copy signature fields (+ optional AS-001 eval). No logging. */
void ae_sig_capture_on_signature(const ae_signature_t *sig);

/**
 * Format next undrained record into out (NUL-terminated CSV row, no prefix).
 * Returns false when nothing to drain.
 */
bool ae_sig_capture_drain_one(char *out, size_t out_cap);

/** Copy diag counters (always safe; zeros when disabled). */
void ae_sig_capture_get_diag(ae_sig_capture_diag_t *out);

/**
 * One-shot markers for UART: return true once when capture becomes ACTIVE /
 * when COMPLETE and fully drained (including overflow stop).
 */
bool ae_sig_capture_take_start_marker(void);
bool ae_sig_capture_take_end_marker(void);

/** Capacity of the fixed capture buffer. */
uint32_t ae_sig_capture_capacity(void);

/** Format one record (host tests / offline). Returns bytes written excluding NUL. */
int ae_sig_capture_format_csv(
    const ae_sig_capture_rec_t *rec,
    uint32_t session_id,
    char *out,
    size_t out_cap
);

#ifdef __cplusplus
}
#endif
