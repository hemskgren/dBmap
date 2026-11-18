/**
 * Shared Swedish public-alert siren evidence (ALARM → SE_PUBLIC_ALERT).
 *
 * Recognizes the continuous monotonic siren timbre shared by FARAN_OVER and
 * VMA. Does **not** distinguish message timing / leaf class.
 *
 * Consumes AE signatures (ESP32 window measurements). Feeds
 * ear_cls_progressive evidence only — does not own Observation create.
 *
 * Capture-grounded notes (EAR-001 indoor HA playback):
 * - 0.8.26 vol 0.60: ~27 consecutive tonal windows, flatness ~0.26–0.33,
 *   dominant hopping ~344/578/688 Hz, mean RMS ~−31 dBFS in mid-tone set.
 * - 0.8.25 vol 0.50: similar persistence, flatter (~0.15–0.24), quieter
 *   (~−42 dBFS) — absolute RMS is not a reliable gate.
 * - 0.8.25 vol 0.12: flatness min ~0.55 — no usable tonal evidence; must
 *   remain unclassified.
 *
 * 5–7 s persistence is treated as a working hypothesis: clear captures show
 * much longer runs; weak captures show none. Thresholds below prefer
 * persistence + flatness + dominant band over absolute level.
 */
#pragma once

#include "ae_types.h"
#include "ear_cls_progressive.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EAR_SIREN_HIST 16u

/**
 * Per-window tonal vote gates (documented; host tests pin these).
 * Flatness upper bound accepts 0.60 capture (~0.29 mean) while rejecting
 * 0.12 noise floor (~0.55+). Dominant band covers observed bin hops.
 */
#define EAR_SIREN_FLAT_MIN 0.12f
#define EAR_SIREN_FLAT_MAX 0.42f
#define EAR_SIREN_DOM_HZ_MIN 300.0f
#define EAR_SIREN_DOM_HZ_MAX 750.0f
#define EAR_SIREN_CREST_MAX_DB 14.0f
#define EAR_SIREN_BG_MARGIN_DB 2.5f

/** Consecutive tonal windows required for ALARM / SE_PUBLIC_ALERT evidence. */
#define EAR_SIREN_ALARM_MIN_STREAK 3u
#define EAR_SIREN_SE_MIN_STREAK 5u

typedef struct {
    bool tonal_vote;
    float flatness;
    float dominant_hz;
    float rms_dbfs;
    uint32_t window_ms;
    int64_t window_end_mono_us;
} ear_siren_hist_t;

/**
 * Result of the most recent ear_siren_se_public_on_signature call.
 * Support for Observation END must use last_kind == USABLE and tonal_vote —
 * never consecutive/streak alone (INVALID leaves consecutive unchanged).
 */
typedef enum {
    EAR_SIREN_LAST_NONE = 0,
    EAR_SIREN_LAST_INVALID = 1,
    EAR_SIREN_LAST_UNUSABLE = 2, /* GAP or other non-usable quality */
    EAR_SIREN_LAST_USABLE = 3,
} ear_siren_last_kind_t;

typedef struct {
    ear_siren_last_kind_t kind;
    /** Meaningful only when kind == EAR_SIREN_LAST_USABLE. */
    bool tonal_vote;
    uint32_t window_ms;
    int64_t window_end_mono_us;
    float rms_dbfs;
    bool has_rms_dbfs;
} ear_siren_last_window_t;

typedef struct {
    bool initialized;
    ear_siren_hist_t hist[EAR_SIREN_HIST];
    uint8_t hist_count;
    uint8_t hist_head;
    uint32_t consecutive;
    uint32_t streak_window_ms_sum;
    float streak_flat_sum;
    float bg_rms_ema;
    bool bg_rms_valid;
    uint32_t windows_seen;
    uint32_t tonal_votes;
    uint32_t evidence_alarm;
    uint32_t evidence_se;
    /** Outcome of the latest on_signature ingest (not a streak summary). */
    ear_siren_last_window_t last_window;
} ear_siren_se_public_t;

void ear_siren_se_public_init(ear_siren_se_public_t *det);

/** Reset streak/history (e.g. engine stop). Keeps bg EMA. */
void ear_siren_se_public_reset_streak(ear_siren_se_public_t *det);

/**
 * Ingest one AE signature window. Ignores NULL / invalid / GAP quality for
 * streak evidence, but always records last_window for current-window support
 * queries (INVALID/GAP → non-USABLE kinds).
 * Updates persistence state used by fill_evidence / allows_promote.
 */
void ear_siren_se_public_on_signature(
    ear_siren_se_public_t *det,
    const ae_signature_t *sig
);

/**
 * Copy the outcome of the most recent on_signature call.
 * Current-window support for AS002 END must use kind==USABLE + tonal_vote;
 * do not treat consecutive>0 as proof the current window contains the signature.
 */
void ear_siren_se_public_get_last_window(
    const ear_siren_se_public_t *det,
    ear_siren_last_window_t *out
);

/**
 * Fill progressive-classification evidence. Never sets FARAN_OVER / VMA.
 * Returns true when at least ALARM evidence is present.
 */
bool ear_siren_se_public_fill_evidence(
    const ear_siren_se_public_t *det,
    ear_cls_evidence_t *out_ev
);

/**
 * True when evidence is strong enough to authorize Observation promote
 * (ALARM streak met). Does not bypass OM/reserve/promote — policy only.
 */
bool ear_siren_se_public_allows_promote(const ear_siren_se_public_t *det);

uint32_t ear_siren_se_public_consecutive(const ear_siren_se_public_t *det);

#ifdef __cplusplus
}
#endif
