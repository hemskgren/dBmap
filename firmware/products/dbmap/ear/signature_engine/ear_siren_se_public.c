/**
 * Shared Swedish public-alert siren evidence — see ear_siren_se_public.h.
 */

#include "ear_siren_se_public.h"

#include <math.h>
#include <string.h>

#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
#include "esp_log.h"
#endif

static float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static bool quality_usable(ae_quality_t q)
{
    return q == AE_QUALITY_OK || q == AE_QUALITY_DEGRADED;
}

static bool window_tonal_vote(
    const ae_signature_t *sig,
    float bg_rms_ema,
    bool bg_valid
#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
    , const char **gate
#endif
)
{
#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
#define TONAL_REJECT(reason) do { *gate = (reason); return false; } while (0)
#else
#define TONAL_REJECT(reason) return false
#endif
    if (sig == NULL || !sig->spectral_valid) {
        TONAL_REJECT("spectral");
    }
    if (!quality_usable(sig->quality)) {
        TONAL_REJECT("quality");
    }
    if (!(isfinite(sig->flatness)
          && isfinite(sig->dominant_hz)
          && isfinite(sig->rms_dbfs)
          && isfinite(sig->crest_db))) {
        TONAL_REJECT("nonfinite");
    }
    if (sig->flatness < EAR_SIREN_FLAT_MIN
        || sig->flatness > EAR_SIREN_FLAT_MAX) {
        TONAL_REJECT("flatness");
    }
    if (sig->dominant_hz < EAR_SIREN_DOM_HZ_MIN
        || sig->dominant_hz > EAR_SIREN_DOM_HZ_MAX) {
        TONAL_REJECT("dominant");
    }
    if (sig->crest_db > EAR_SIREN_CREST_MAX_DB) {
        TONAL_REJECT("crest");
    }
    /*
     * Relative level vs recent non-tonal background. Once a streak has
     * started, skip the margin so indoor quiet captures (vol 0.50 ~−42
     * dBFS) remain contiguous when bg also rises.
     */
    if (bg_valid && sig->rms_dbfs < (bg_rms_ema + EAR_SIREN_BG_MARGIN_DB)) {
        TONAL_REJECT("rms");
    }
#undef TONAL_REJECT
    return true;
}

void ear_siren_se_public_init(ear_siren_se_public_t *det)
{
    if (det == NULL) {
        return;
    }
    memset(det, 0, sizeof(*det));
    det->initialized = true;
    det->bg_rms_ema = -60.0f;
    det->bg_rms_valid = false;
}

void ear_siren_se_public_reset_streak(ear_siren_se_public_t *det)
{
    if (det == NULL) {
        return;
    }
    memset(det->hist, 0, sizeof(det->hist));
    det->hist_count = 0;
    det->hist_head = 0;
    det->consecutive = 0;
    det->streak_window_ms_sum = 0;
    det->streak_flat_sum = 0.0f;
}

static void push_hist(ear_siren_se_public_t *det, const ear_siren_hist_t *h)
{
    det->hist[det->hist_head] = *h;
    det->hist_head = (uint8_t)((det->hist_head + 1u) % EAR_SIREN_HIST);
    if (det->hist_count < EAR_SIREN_HIST) {
        det->hist_count++;
    }
}

static void set_last_window(
    ear_siren_se_public_t *det,
    ear_siren_last_kind_t kind,
    bool tonal_vote,
    const ae_signature_t *sig
)
{
    memset(&det->last_window, 0, sizeof(det->last_window));
    det->last_window.kind = kind;
    det->last_window.tonal_vote = tonal_vote;
    if (sig != NULL) {
        det->last_window.window_ms =
            sig->window_ms > 0u ? sig->window_ms : 1000u;
        det->last_window.window_end_mono_us = sig->window_end_mono_us;
        if (isfinite(sig->rms_dbfs)) {
            det->last_window.rms_dbfs = sig->rms_dbfs;
            det->last_window.has_rms_dbfs = true;
        }
    }
}

void ear_siren_se_public_on_signature(
    ear_siren_se_public_t *det,
    const ae_signature_t *sig
)
{
    if (det == NULL || !det->initialized || sig == NULL) {
        return;
    }
#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
    const unsigned before = det->consecutive;
    const bool bg_valid = det->bg_rms_valid;
    const double bg = bg_valid ? (double)det->bg_rms_ema : NAN;
    const char *gate = "tonal";
#define LOG_TONAL(reason, level_checked) \
    ESP_LOGI("ear_dec", "AS002 seq=%lu gate=%s rms=%.2f bg=%.2f bg_ok=%d level=%d streak=%u>%u allow=%d", \
             (unsigned long)sig->window_seq, (reason), (double)sig->rms_dbfs, bg, bg_valid, \
             (level_checked), before, (unsigned)det->consecutive, \
             ear_siren_se_public_allows_promote(det))
#else
#define LOG_TONAL(reason, level_checked) ((void)0)
#endif
    if (sig->quality == AE_QUALITY_INVALID) {
        /* Unknown evidence: do not mutate streak; record non-usable last. */
        set_last_window(det, EAR_SIREN_LAST_INVALID, false, sig);
        LOG_TONAL("invalid", false);
        return;
    }
    if (sig->quality == AE_QUALITY_GAP) {
        /* Treat gap as interruption of tonal continuity. */
        det->consecutive = 0;
        det->streak_window_ms_sum = 0;
        det->streak_flat_sum = 0.0f;
        set_last_window(det, EAR_SIREN_LAST_UNUSABLE, false, sig);
        LOG_TONAL("gap", false);
        return;
    }
    if (!quality_usable(sig->quality)) {
        set_last_window(det, EAR_SIREN_LAST_UNUSABLE, false, sig);
        LOG_TONAL("quality", false);
        return;
    }

    det->windows_seen++;

    const bool in_streak = (det->consecutive > 0u);
    const bool vote = window_tonal_vote(
        sig,
        det->bg_rms_ema,
        det->bg_rms_valid && !in_streak
#if CONFIG_DBMAP_EAR_OBS_DECISION_DIAG
        , &gate
#endif
    );

    ear_siren_hist_t h;
    memset(&h, 0, sizeof(h));
    h.tonal_vote = vote;
    h.flatness = sig->flatness;
    h.dominant_hz = sig->dominant_hz;
    h.rms_dbfs = sig->rms_dbfs;
    h.window_ms = sig->window_ms > 0u ? sig->window_ms : 1000u;
    h.window_end_mono_us = sig->window_end_mono_us;
    push_hist(det, &h);
    set_last_window(det, EAR_SIREN_LAST_USABLE, vote, sig);

    if (vote) {
        det->tonal_votes++;
        det->consecutive++;
        det->streak_window_ms_sum += h.window_ms;
        det->streak_flat_sum += sig->flatness;
        if (det->consecutive >= EAR_SIREN_ALARM_MIN_STREAK) {
            det->evidence_alarm++;
        }
        if (det->consecutive >= EAR_SIREN_SE_MIN_STREAK) {
            det->evidence_se++;
        }
    } else {
        det->consecutive = 0;
        det->streak_window_ms_sum = 0;
        det->streak_flat_sum = 0.0f;
        /* Update background from non-tonal windows only. */
        if (isfinite(sig->rms_dbfs)) {
            if (!det->bg_rms_valid) {
                det->bg_rms_ema = sig->rms_dbfs;
                det->bg_rms_valid = true;
            } else {
                det->bg_rms_ema =
                    0.85f * det->bg_rms_ema + 0.15f * sig->rms_dbfs;
            }
        }
    }
    LOG_TONAL(gate, bg_valid && !in_streak
              && (vote || !strcmp(gate, "rms")));
#undef LOG_TONAL
}

void ear_siren_se_public_get_last_window(
    const ear_siren_se_public_t *det,
    ear_siren_last_window_t *out
)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (det == NULL || !det->initialized) {
        return;
    }
    *out = det->last_window;
}

bool ear_siren_se_public_fill_evidence(
    const ear_siren_se_public_t *det,
    ear_cls_evidence_t *out_ev
)
{
    if (out_ev == NULL) {
        return false;
    }
    memset(out_ev, 0, sizeof(*out_ev));
    if (det == NULL || !det->initialized) {
        return false;
    }
    if (det->consecutive < EAR_SIREN_ALARM_MIN_STREAK) {
        return false;
    }

    const float mean_flat =
        (det->consecutive > 0u)
            ? (det->streak_flat_sum / (float)det->consecutive)
            : 1.0f;
    const float flat_bonus = clampf(0.42f - mean_flat, 0.0f, 0.30f);
    const float streak_f = (float)det->consecutive;

    /* Independent per-level confidences (ear_hyp_v2 contract). */
    out_ev->alarm_ok = true;
    out_ev->alarm_confidence = clampf(
        0.55f + 0.06f * streak_f + 0.5f * flat_bonus, 0.55f, 0.95f
    );

    if (det->consecutive >= EAR_SIREN_SE_MIN_STREAK) {
        out_ev->se_public_alert_ok = true;
        out_ev->se_public_alert_confidence = clampf(
            0.60f + 0.05f * streak_f + 0.6f * flat_bonus, 0.60f, 0.95f
        );
    }

    /* Leaf slots intentionally unset — shared signature only. */
    return true;
}

bool ear_siren_se_public_allows_promote(const ear_siren_se_public_t *det)
{
    return det != NULL && det->initialized
        && det->consecutive >= EAR_SIREN_ALARM_MIN_STREAK;
}

uint32_t ear_siren_se_public_consecutive(const ear_siren_se_public_t *det)
{
    return (det != NULL) ? det->consecutive : 0u;
}
