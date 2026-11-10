#include "dbmap_activity_detector.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "sdkconfig.h"
#if defined(CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG) && CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
#define DBMAP_ACTIVITY_SERIAL_DIAG 1
static const char *TAG = "ear_act";
#endif
#endif

#ifndef DBMAP_ACTIVITY_SERIAL_DIAG
#define DBMAP_ACTIVITY_SERIAL_DIAG 0
#endif

/** Bounded one-shot log of first usable windows after boot settle. */
#define DBMAP_ACTIVITY_POST_SETTLE_DIAG_MAX 5u

static void clear_exit_hold(dbmap_activity_detector_t *det)
{
    det->exit_hold_elapsed_us = 0;
    det->exit_hold_anchor_us = 0;
    det->exit_hold_anchor_seq = 0;
    det->exit_hold_anchor_valid = false;
}

static void note_exit_hold_peak(dbmap_activity_detector_t *det)
{
    if (det->exit_hold_elapsed_us > det->exit_hold_elapsed_us_max) {
        det->exit_hold_elapsed_us_max = det->exit_hold_elapsed_us;
    }
}

static void invalidate_exit_hold_anchor(dbmap_activity_detector_t *det)
{
    det->exit_hold_anchor_valid = false;
}

static void begin_exit_hold(dbmap_activity_detector_t *det, int64_t t, uint32_t seq)
{
    det->exit_hold_elapsed_us = 0;
    det->exit_hold_anchor_us = t;
    det->exit_hold_anchor_seq = seq;
    det->exit_hold_anchor_valid = true;
}

static void add_exit_hold_elapsed(dbmap_activity_detector_t *det, int64_t delta_us)
{
    if (delta_us <= 0) {
        return;
    }
    if (det->exit_hold_elapsed_us > INT64_MAX - delta_us) {
        det->exit_hold_elapsed_us = INT64_MAX;
    } else {
        det->exit_hold_elapsed_us += delta_us;
    }
    note_exit_hold_peak(det);
}

/** True when cur is the immediate successor of prev (no uint32 wrap). */
static bool seq_is_consecutive(uint32_t prev, uint32_t cur)
{
    return prev != UINT32_MAX && cur == prev + 1u;
}

bool dbmap_activity_config_valid(const dbmap_activity_config_t *cfg)
{
    if (cfg == NULL) {
        return false;
    }
    if (!isfinite((double)cfg->enter_threshold_dbfs) || !isfinite((double)cfg->exit_threshold_dbfs)) {
        return false;
    }
    if (cfg->enter_threshold_dbfs > 0.0f || cfg->exit_threshold_dbfs > 0.0f) {
        return false;
    }
    /* Louder = higher dBFS. Enter threshold must be strictly above exit. */
    if (cfg->enter_threshold_dbfs <= cfg->exit_threshold_dbfs) {
        return false;
    }
    if (cfg->min_activity_ms == 0 || cfg->exit_hold_ms == 0 || cfg->observation_interval_ms == 0) {
        return false;
    }
    if (cfg->relative_end_enabled) {
        if (!isfinite((double)cfg->return_margin_db) || cfg->return_margin_db < 0.0f) {
            return false;
        }
        if (!isfinite((double)cfg->cancel_hysteresis_db) || cfg->cancel_hysteresis_db < 0.0f) {
            return false;
        }
        if (!isfinite((double)cfg->min_event_contrast_db) || cfg->min_event_contrast_db < 0.0f) {
            return false;
        }
        if (cfg->relative_exit_hold_ms == 0) {
            return false;
        }
    }
    return true;
}

static void relative_clear_episode(dbmap_activity_detector_t *det)
{
    det->baseline_frozen = false;
    det->baseline_reliable = false;
    det->frozen_baseline_dbfs = 0.0f;
    det->frozen_baseline_n = 0;
    det->relative_armed = false;
    det->arming_count = 0;
    det->measured_contrast_db = 0.0f;
    det->exit_latch = DBMAP_ACTIVITY_EXIT_LATCH_NONE;
    memset(det->arming_rms, 0, sizeof(det->arming_rms));
}

static void relative_clear_all(dbmap_activity_detector_t *det)
{
    relative_clear_episode(det);
    det->baseline_head = 0;
    det->baseline_count = 0;
    memset(det->baseline_entries, 0, sizeof(det->baseline_entries));
}

void dbmap_activity_detector_init(dbmap_activity_detector_t *det, const dbmap_activity_config_t *cfg)
{
    if (det == NULL) {
        return;
    }
    memset(det, 0, sizeof(*det));
    if (cfg != NULL) {
        det->cfg = *cfg;
    }
    det->state = DBMAP_ACTIVITY_IDLE;
    det->boot_complete = (det->cfg.boot_settle_ms == 0);
}

bool dbmap_activity_sample_usable(const dbmap_activity_sample_t *sample)
{
    if (sample == NULL || !sample->has_measurement) {
        return false;
    }
    if (!isfinite((double)sample->rms_dbfs) || sample->rms_dbfs > 0.0f) {
        return false;
    }
    if (sample->transport_status == NULL || sample->microphone_status == NULL) {
        return false;
    }
    /* Technical health: I2S fill must be usable. */
    if (strcmp(sample->transport_status, "ok") != 0 &&
        strcmp(sample->transport_status, "degraded") != 0) {
        return false;
    }
    /*
     * Reject hard mic faults only. microphone_status comes from
     * dbmap_audio_classify_microphone (structure diagnostics):
     *   unavailable / disconnected_suspected / noise_suspected → unusable
     *   signal_present / unknown → usable (RMS thresholds decide activity)
     *
     * Requiring signal_present alone would drop weak-but-valid windows that
     * classify as unknown (ambiguous_window).
     */
    if (strcmp(sample->microphone_status, "unavailable") == 0 ||
        strcmp(sample->microphone_status, "disconnected_suspected") == 0 ||
        strcmp(sample->microphone_status, "noise_suspected") == 0) {
        return false;
    }
    return true;
}

static dbmap_activity_emission_t make_emission(
    dbmap_activity_emit_kind_t kind,
    dbmap_activity_termination_t termination,
    float level,
    int64_t start_us,
    int64_t end_us
)
{
    dbmap_activity_emission_t out;
    memset(&out, 0, sizeof(out));
    out.kind = kind;
    out.termination = termination;
    out.signal_level_dbfs = level > 0.0f ? 0.0f : level;
    out.interval_start_mono_us = start_us;
    out.interval_end_mono_us = end_us;
    if (end_us >= start_us) {
        out.duration_ms = (double)(end_us - start_us) / 1000.0;
    } else {
        out.duration_ms = 0.0;
    }
    return out;
}

static bool above_enter(const dbmap_activity_detector_t *det, float rms)
{
    return rms >= det->cfg.enter_threshold_dbfs;
}

static bool below_exit_abs(const dbmap_activity_detector_t *det, float rms)
{
    return rms < det->cfg.exit_threshold_dbfs;
}

static float baseline_median(const float *values, uint32_t n)
{
    float tmp[DBMAP_ACTIVITY_BASELINE_CAP];
    if (values == NULL || n == 0) {
        return 0.0f;
    }
    if (n > DBMAP_ACTIVITY_BASELINE_CAP) {
        n = DBMAP_ACTIVITY_BASELINE_CAP;
    }
    memcpy(tmp, values, sizeof(float) * n);
    for (uint32_t i = 1; i < n; ++i) {
        float key = tmp[i];
        int j = (int)i - 1;
        while (j >= 0 && tmp[j] > key) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
    }
    if ((n % 2u) == 1u) {
        return tmp[n / 2u];
    }
    return 0.5f * (tmp[n / 2u - 1u] + tmp[n / 2u]);
}

static void baseline_drop_stale(dbmap_activity_detector_t *det, int64_t now_us)
{
    if (!det->cfg.relative_end_enabled || det->cfg.baseline_stale_ms == 0 || det->baseline_count == 0) {
        return;
    }
    const int64_t cutoff = now_us - ((int64_t)det->cfg.baseline_stale_ms * 1000LL);
    while (det->baseline_count > 0) {
        const uint32_t oldest =
            (det->baseline_head + DBMAP_ACTIVITY_BASELINE_CAP - det->baseline_count)
            % DBMAP_ACTIVITY_BASELINE_CAP;
        if (det->baseline_entries[oldest].t_end_mono_us >= cutoff) {
            break;
        }
        det->baseline_entries[oldest].valid = false;
        det->baseline_count--;
    }
}

static void baseline_push(
    dbmap_activity_detector_t *det,
    float rms,
    int64_t t_end_us,
    uint32_t window_seq
)
{
    if (!det->cfg.relative_end_enabled || det->baseline_frozen) {
        return;
    }
    if (!isfinite((double)rms) || rms > 0.0f || t_end_us < 0) {
        return;
    }
    baseline_drop_stale(det, t_end_us);
    if (det->baseline_count == DBMAP_ACTIVITY_BASELINE_CAP) {
        det->baseline_count--;
    }
    det->baseline_entries[det->baseline_head].rms_dbfs = rms;
    det->baseline_entries[det->baseline_head].t_end_mono_us = t_end_us;
    det->baseline_entries[det->baseline_head].window_seq = window_seq;
    det->baseline_entries[det->baseline_head].valid = true;
    det->baseline_head = (det->baseline_head + 1u) % DBMAP_ACTIVITY_BASELINE_CAP;
    det->baseline_count++;
}

static void baseline_freeze(dbmap_activity_detector_t *det, int64_t episode_start_us)
{
    if (!det->cfg.relative_end_enabled) {
        return;
    }
    baseline_drop_stale(det, episode_start_us);

    float vals[DBMAP_ACTIVITY_BASELINE_CAP];
    uint32_t n = 0;
#if DBMAP_ACTIVITY_SERIAL_DIAG
    uint32_t accepted_seq[DBMAP_ACTIVITY_BASELINE_CAP];
    int64_t accepted_t[DBMAP_ACTIVITY_BASELINE_CAP];
    float accepted_rms[DBMAP_ACTIVITY_BASELINE_CAP];
#endif
    for (uint32_t i = 0; i < det->baseline_count; ++i) {
        const uint32_t idx =
            (det->baseline_head + DBMAP_ACTIVITY_BASELINE_CAP - det->baseline_count + i)
            % DBMAP_ACTIVITY_BASELINE_CAP;
        const dbmap_activity_baseline_sample_t *s = &det->baseline_entries[idx];
        if (!s->valid) {
            continue;
        }
        /* Pre-event only: window ended at or before episode start. */
        if (s->t_end_mono_us > episode_start_us) {
            continue;
        }
#if DBMAP_ACTIVITY_SERIAL_DIAG
        accepted_rms[n] = s->rms_dbfs;
        accepted_t[n] = s->t_end_mono_us;
        accepted_seq[n] = s->window_seq;
#endif
        vals[n++] = s->rms_dbfs;
    }

    det->baseline_frozen = true;
    det->frozen_baseline_n = n;
    if (n >= DBMAP_ACTIVITY_BASELINE_MIN_SAMPLES) {
        det->baseline_reliable = true;
        det->frozen_baseline_dbfs = baseline_median(vals, n);
    } else {
        det->baseline_reliable = false;
        det->frozen_baseline_dbfs = 0.0f;
    }

#if DBMAP_ACTIVITY_SERIAL_DIAG
    ESP_LOGI(
        TAG,
        "diag baseline_freeze n=%lu reliable=%d base=%.2f episode_start_us=%lld",
        (unsigned long)n,
        det->baseline_reliable ? 1 : 0,
        (double)det->frozen_baseline_dbfs,
        (long long)episode_start_us
    );
    for (uint32_t i = 0; i < n; ++i) {
        ESP_LOGI(
            TAG,
            "diag baseline_sample i=%lu/%lu rms_dbfs=%.2f t_end_us=%lld win_seq=%lu",
            (unsigned long)(i + 1u),
            (unsigned long)n,
            (double)accepted_rms[i],
            (long long)accepted_t[i],
            (unsigned long)accepted_seq[i]
        );
    }
#endif
}

static void try_arm_relative(dbmap_activity_detector_t *det)
{
    if (!det->cfg.relative_end_enabled || !det->baseline_frozen || !det->baseline_reliable) {
        det->relative_armed = false;
        return;
    }
    if (det->relative_armed || det->arming_count == 0) {
        return;
    }
    float peak = det->arming_rms[0];
    for (uint32_t i = 1; i < det->arming_count; ++i) {
        if (det->arming_rms[i] > peak) {
            peak = det->arming_rms[i];
        }
    }
    det->measured_contrast_db = peak - det->frozen_baseline_dbfs;
    if (det->measured_contrast_db >= det->cfg.min_event_contrast_db) {
        det->relative_armed = true;
    }
}

/**
 * ACTIVE only: collect arming RMS then evaluate contrast (call before exit check).
 * Contrast uses the max over the first two usable episode windows; arming is
 * decided only once both samples are present (or already armed).
 */
static void note_arming_rms(dbmap_activity_detector_t *det, float rms)
{
    if (!det->cfg.relative_end_enabled || !det->baseline_frozen || !det->baseline_reliable) {
        return;
    }
    if (det->relative_armed) {
        return;
    }
    if (det->arming_count < DBMAP_ACTIVITY_ARMING_WINDOWS) {
        det->arming_rms[det->arming_count++] = rms;
    }
    if (det->arming_count >= DBMAP_ACTIVITY_ARMING_WINDOWS) {
        try_arm_relative(det);
    }
}

static void on_episode_start(dbmap_activity_detector_t *det, int64_t episode_start_us, float start_rms)
{
    if (!det->cfg.relative_end_enabled) {
        return;
    }
    det->relative_armed = false;
    det->arming_count = 0;
    det->measured_contrast_db = 0.0f;
    det->exit_latch = DBMAP_ACTIVITY_EXIT_LATCH_NONE;
    memset(det->arming_rms, 0, sizeof(det->arming_rms));
    baseline_freeze(det, episode_start_us);
    note_arming_rms(det, start_rms);
}

static void on_episode_end_cleanup(dbmap_activity_detector_t *det)
{
    clear_exit_hold(det);
    if (!det->cfg.relative_end_enabled) {
        det->exit_latch = DBMAP_ACTIVITY_EXIT_LATCH_NONE;
        return;
    }
    /* Clear freeze and ring so the next episode collects a fresh baseline. */
    relative_clear_all(det);
}

static bool below_rel_entry(const dbmap_activity_detector_t *det, float rms)
{
    return rms <= det->frozen_baseline_dbfs + det->cfg.return_margin_db;
}

static bool stay_rel_exit(const dbmap_activity_detector_t *det, float rms)
{
    return rms <= det->frozen_baseline_dbfs + det->cfg.return_margin_db
                      + det->cfg.cancel_hysteresis_db;
}

static bool should_enter_pending_exit(const dbmap_activity_detector_t *det, float rms)
{
    if (det->cfg.relative_end_enabled && det->relative_armed) {
        return below_exit_abs(det, rms) || below_rel_entry(det, rms);
    }
    return below_exit_abs(det, rms);
}

static bool should_stay_pending_exit(const dbmap_activity_detector_t *det, float rms)
{
    if (det->exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE) {
        return stay_rel_exit(det, rms);
    }
    /* Absolute latch (or NONE should not occur in PENDING_EXIT). */
    return below_exit_abs(det, rms);
}

static uint32_t latched_exit_hold_ms(const dbmap_activity_detector_t *det)
{
    if (det->exit_latch == DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE) {
        return det->cfg.relative_exit_hold_ms;
    }
    return det->cfg.exit_hold_ms;
}

static dbmap_activity_emission_t maybe_periodic(
    dbmap_activity_detector_t *det,
    float rms,
    int64_t t
)
{
    int64_t due = det->last_emit_us + ((int64_t)det->cfg.observation_interval_ms * 1000LL);
    if (t >= due) {
        int64_t start = det->last_emit_us;
        det->last_emit_us = t;
        det->emissions_periodic += 1;
        return make_emission(
            DBMAP_ACTIVITY_EMIT_PERIODIC, DBMAP_ACTIVITY_TERM_NONE, rms, start, t
        );
    }
    dbmap_activity_emission_t none;
    memset(&none, 0, sizeof(none));
    none.kind = DBMAP_ACTIVITY_EMIT_NONE;
    return none;
}

static dbmap_activity_emission_t force_end_after_invalid(dbmap_activity_detector_t *det)
{
    int64_t end_us = det->has_last_valid ? det->last_valid_us : det->last_emit_us;
    int64_t start_us = det->last_emit_us;
    float level = det->last_level_dbfs;
    det->state = DBMAP_ACTIVITY_IDLE;
    det->pending_since_us = 0;
    det->last_emit_us = end_us;
    det->emissions_end += 1;
    det->forced_ends_invalid += 1;
    on_episode_end_cleanup(det);
    return make_emission(
        DBMAP_ACTIVITY_EMIT_END, DBMAP_ACTIVITY_TERM_INVALID_HOLD, level, start_us, end_us
    );
}

dbmap_activity_state_t dbmap_activity_detector_state(const dbmap_activity_detector_t *det)
{
    return det != NULL ? det->state : DBMAP_ACTIVITY_IDLE;
}

void dbmap_activity_detector_fill_diag(
    const dbmap_activity_detector_t *det,
    dbmap_activity_diag_t *out
)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (det == NULL) {
        return;
    }
    out->state = det->state;
    out->relative_end_enabled = det->cfg.relative_end_enabled;
    out->relative_armed = det->relative_armed;
    out->baseline_frozen = det->baseline_frozen;
    out->baseline_reliable = det->baseline_reliable;
    out->frozen_baseline_dbfs = det->frozen_baseline_dbfs;
    out->frozen_baseline_n = det->frozen_baseline_n;
    out->measured_contrast_db = det->measured_contrast_db;
    out->abs_enter_dbfs = det->cfg.enter_threshold_dbfs;
    out->abs_exit_dbfs = det->cfg.exit_threshold_dbfs;
    out->rel_thresholds_valid =
        det->cfg.relative_end_enabled && det->baseline_frozen && det->baseline_reliable;
    if (out->rel_thresholds_valid) {
        /* Same expressions as below_rel_entry / stay_rel_exit. */
        out->rel_exit_dbfs =
            det->frozen_baseline_dbfs + det->cfg.return_margin_db;
        out->rel_cancel_dbfs =
            det->frozen_baseline_dbfs + det->cfg.return_margin_db
            + det->cfg.cancel_hysteresis_db;
    }
    out->exit_latch = det->exit_latch;
    out->exit_hold_elapsed_us = det->exit_hold_elapsed_us;
    out->exit_hold_elapsed_us_max = det->exit_hold_elapsed_us_max;
    out->pending_exit_enters = det->pending_exit_enters;
    out->pending_exit_cancels = det->pending_exit_cancels;
    out->latched_hold_ms = latched_exit_hold_ms(det);
}

const char *dbmap_activity_exit_latch_name(dbmap_activity_exit_latch_t latch)
{
    switch (latch) {
    case DBMAP_ACTIVITY_EXIT_LATCH_ABSOLUTE:
        return "absolute";
    case DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE:
        return "relative";
    case DBMAP_ACTIVITY_EXIT_LATCH_NONE:
    default:
        return "none";
    }
}

static const char *activity_state_short_name(dbmap_activity_state_t state)
{
    switch (state) {
    case DBMAP_ACTIVITY_PENDING_ENTER:
        return "PENDING_ENTER";
    case DBMAP_ACTIVITY_ACTIVE:
        return "ACTIVE";
    case DBMAP_ACTIVITY_PENDING_EXIT:
        return "PENDING_EXIT";
    case DBMAP_ACTIVITY_IDLE:
    default:
        return "IDLE";
    }
}

int dbmap_activity_format_rel_diag(const dbmap_activity_diag_t *diag, char *buf, size_t buflen)
{
    if (diag == NULL || buf == NULL || buflen == 0) {
        return -1;
    }
    if (!diag->relative_end_enabled) {
        return snprintf(
            buf,
            buflen,
            "rel_end=0 state=%s",
            activity_state_short_name(diag->state)
        );
    }
    if (diag->rel_thresholds_valid) {
        return snprintf(
            buf,
            buflen,
            "rel_end=1 state=%s armed=%d base=%.2f base_n=%lu base_ok=%d "
            "rel_exit=%.2f rel_cancel=%.2f contrast=%.2f "
            "latch=%s hold_ms=%lu hold_el_ms=%lld hold_max_ms=%lld "
            "pend_enter=%lu pend_cancel=%lu",
            activity_state_short_name(diag->state),
            diag->relative_armed ? 1 : 0,
            (double)diag->frozen_baseline_dbfs,
            (unsigned long)diag->frozen_baseline_n,
            diag->baseline_reliable ? 1 : 0,
            (double)diag->rel_exit_dbfs,
            (double)diag->rel_cancel_dbfs,
            (double)diag->measured_contrast_db,
            dbmap_activity_exit_latch_name(diag->exit_latch),
            (unsigned long)diag->latched_hold_ms,
            (long long)(diag->exit_hold_elapsed_us / 1000LL),
            (long long)(diag->exit_hold_elapsed_us_max / 1000LL),
            (unsigned long)diag->pending_exit_enters,
            (unsigned long)diag->pending_exit_cancels
        );
    }
    return snprintf(
        buf,
        buflen,
        "rel_end=1 state=%s armed=%d base=n/a base_n=%lu base_ok=%d "
        "rel_exit=n/a rel_cancel=n/a contrast=%.2f "
        "latch=%s hold_ms=%lu hold_el_ms=%lld hold_max_ms=%lld "
        "pend_enter=%lu pend_cancel=%lu",
        activity_state_short_name(diag->state),
        diag->relative_armed ? 1 : 0,
        (unsigned long)diag->frozen_baseline_n,
        diag->baseline_reliable ? 1 : 0,
        (double)diag->measured_contrast_db,
        dbmap_activity_exit_latch_name(diag->exit_latch),
        (unsigned long)diag->latched_hold_ms,
        (long long)(diag->exit_hold_elapsed_us / 1000LL),
        (long long)(diag->exit_hold_elapsed_us_max / 1000LL),
        (unsigned long)diag->pending_exit_enters,
        (unsigned long)diag->pending_exit_cancels
    );
}

dbmap_activity_emission_t dbmap_activity_detector_feed(
    dbmap_activity_detector_t *det,
    const dbmap_activity_sample_t *sample
)
{
    dbmap_activity_emission_t none;
    memset(&none, 0, sizeof(none));
    none.kind = DBMAP_ACTIVITY_EMIT_NONE;

    if (det == NULL || sample == NULL || !dbmap_activity_config_valid(&det->cfg)) {
        return none;
    }
    if (!sample->has_measurement) {
        return none;
    }

    /* One decision per completed window. */
    if (det->has_last_window && sample->window_seq == det->last_window_seq) {
        return none;
    }

    /*
     * Missing / backward sequence while PENDING_EXIT: pause natural hold
     * (invalidate anchor only). Detect before updating last_window_seq.
     * Wrap-safe: producer saturates at UINT32_MAX; never use last+1 when last==MAX.
     */
    if (det->has_last_window && det->state == DBMAP_ACTIVITY_PENDING_EXIT) {
        const uint32_t last = det->last_window_seq;
        const uint32_t cur = sample->window_seq;
        if (cur < last) {
            invalidate_exit_hold_anchor(det);
        } else if (last != UINT32_MAX && cur > last + 1u) {
            invalidate_exit_hold_anchor(det);
        }
    }

    det->has_last_window = true;
    det->last_window_seq = sample->window_seq;
    det->windows_seen += 1;

    if (!det->boot_complete) {
        if (sample->ms_since_i2s_enable < det->cfg.boot_settle_ms) {
            det->windows_boot_settle += 1;
            return none;
        }
        det->boot_complete = true;
    }

    if (!dbmap_activity_sample_usable(sample)) {
        det->windows_invalid += 1;
        /*
         * Invalid / missing quality is NOT acoustic silence:
         * - pause natural exit hold (invalidate anchor; keep elapsed)
         * - do not emit periodic
         * - do not drop to IDLE as "quiet"
         * - do not change exit_latch / arming
         */
        if (det->state == DBMAP_ACTIVITY_PENDING_EXIT) {
            invalidate_exit_hold_anchor(det);
        }
        if (det->state == DBMAP_ACTIVITY_PENDING_ENTER) {
            if (det->cfg.max_invalid_hold_ms > 0 && det->has_last_valid &&
                (sample->measured_at_us - det->last_valid_us) >=
                    ((int64_t)det->cfg.max_invalid_hold_ms * 1000LL)) {
                det->state = DBMAP_ACTIVITY_IDLE;
                det->pending_since_us = 0;
            } else if (det->cfg.max_invalid_hold_ms > 0 && !det->has_last_valid &&
                       (sample->measured_at_us - det->pending_since_us) >=
                           ((int64_t)det->cfg.max_invalid_hold_ms * 1000LL)) {
                det->state = DBMAP_ACTIVITY_IDLE;
                det->pending_since_us = 0;
            }
            return none;
        }
        if ((det->state == DBMAP_ACTIVITY_ACTIVE || det->state == DBMAP_ACTIVITY_PENDING_EXIT) &&
            det->cfg.max_invalid_hold_ms > 0 && det->has_last_valid &&
            (sample->measured_at_us - det->last_valid_us) >=
                ((int64_t)det->cfg.max_invalid_hold_ms * 1000LL)) {
            return force_end_after_invalid(det);
        }
        return none;
    }

    const float rms = sample->rms_dbfs;
    const int64_t t = sample->measured_at_us;
    const uint32_t window_ms = sample->window_ms > 0 ? sample->window_ms : 1;
    const int64_t window_start = t - ((int64_t)window_ms * 1000LL);
    det->last_level_dbfs = rms;
    det->last_valid_us = t;
    det->has_last_valid = true;

#if DBMAP_ACTIVITY_SERIAL_DIAG
    /* One-shot: first few usable windows after boot settle (any state). */
    if (det->post_settle_usable_diag_n < DBMAP_ACTIVITY_POST_SETTLE_DIAG_MAX) {
        det->post_settle_usable_diag_n += 1;
        ESP_LOGI(
            TAG,
            "diag post_settle i=%lu/%u rms_dbfs=%.2f peak_dbfs=%.2f "
            "win_seq=%lu t_us=%lld ms_since_i2s=%lu xport=%s mic=%s",
            (unsigned long)det->post_settle_usable_diag_n,
            (unsigned)DBMAP_ACTIVITY_POST_SETTLE_DIAG_MAX,
            (double)rms,
            (double)sample->peak_dbfs,
            (unsigned long)sample->window_seq,
            (long long)t,
            (unsigned long)sample->ms_since_i2s_enable,
            sample->transport_status != NULL ? sample->transport_status : "n/a",
            sample->microphone_status != NULL ? sample->microphone_status : "n/a"
        );
    }
#endif

    /* Baseline: IDLE and below enter only. */
    if (det->state == DBMAP_ACTIVITY_IDLE && !above_enter(det, rms)) {
        baseline_push(det, rms, t, sample->window_seq);
    }

    switch (det->state) {
    case DBMAP_ACTIVITY_IDLE:
        if (above_enter(det, rms)) {
            det->pending_since_us = window_start;
            det->episode_start_us = window_start;
            if ((t - window_start) >= ((int64_t)det->cfg.min_activity_ms * 1000LL)) {
                det->state = DBMAP_ACTIVITY_ACTIVE;
                det->last_emit_us = t;
                det->emissions_start += 1;
                on_episode_start(det, det->episode_start_us, rms);
                return make_emission(
                    DBMAP_ACTIVITY_EMIT_START,
                    DBMAP_ACTIVITY_TERM_NONE,
                    rms,
                    det->episode_start_us,
                    t
                );
            }
            det->state = DBMAP_ACTIVITY_PENDING_ENTER;
        }
        return none;

    case DBMAP_ACTIVITY_PENDING_ENTER:
        if (!above_enter(det, rms)) {
            det->state = DBMAP_ACTIVITY_IDLE;
            det->pending_since_us = 0;
            return none;
        }
        if ((t - det->pending_since_us) >= ((int64_t)det->cfg.min_activity_ms * 1000LL)) {
            det->state = DBMAP_ACTIVITY_ACTIVE;
            det->episode_start_us = det->pending_since_us;
            det->last_emit_us = t;
            det->emissions_start += 1;
            on_episode_start(det, det->episode_start_us, rms);
            return make_emission(
                DBMAP_ACTIVITY_EMIT_START,
                DBMAP_ACTIVITY_TERM_NONE,
                rms,
                det->episode_start_us,
                t
            );
        }
        return none;

    case DBMAP_ACTIVITY_ACTIVE:
        /* Call order: arm before exit-entry evaluation. */
        note_arming_rms(det, rms);
        if (should_enter_pending_exit(det, rms)) {
            /* Latch follows the entry cause: relative only when relative entry
             * is met (and armed). Absolute-only entry keeps the absolute latch
             * even if Relative END is already armed. Both true → relative. */
            det->exit_latch =
                (det->cfg.relative_end_enabled && det->relative_armed
                 && below_rel_entry(det, rms))
                    ? DBMAP_ACTIVITY_EXIT_LATCH_RELATIVE
                    : DBMAP_ACTIVITY_EXIT_LATCH_ABSOLUTE;
            det->state = DBMAP_ACTIVITY_PENDING_EXIT;
            det->pending_since_us = t; /* diagnostic only; END uses exit_hold_elapsed_us */
            det->pending_exit_enters += 1;
            begin_exit_hold(det, t, sample->window_seq);
        }
        return maybe_periodic(det, rms, t);

    case DBMAP_ACTIVITY_PENDING_EXIT:
        /* Do not re-arm or change latch while exiting. */
        if (!should_stay_pending_exit(det, rms)) {
            note_exit_hold_peak(det);
            det->state = DBMAP_ACTIVITY_ACTIVE;
            det->pending_since_us = 0;
            det->exit_latch = DBMAP_ACTIVITY_EXIT_LATCH_NONE;
            det->pending_exit_cancels += 1;
            clear_exit_hold(det);
            return maybe_periodic(det, rms, t);
        }

        {
            const int64_t contig_max_us = (int64_t)window_ms * 1000 * 3 / 2;
            const bool seq_ok =
                det->exit_hold_anchor_valid
                && seq_is_consecutive(det->exit_hold_anchor_seq, sample->window_seq);
            const bool time_ok =
                det->exit_hold_anchor_valid && t >= det->exit_hold_anchor_us;
            const int64_t delta_us =
                time_ok ? (t - det->exit_hold_anchor_us) : (int64_t)0;
            const bool continuous =
                seq_ok && time_ok && delta_us > 0 && delta_us <= contig_max_us;

            if (continuous) {
                add_exit_hold_elapsed(det, delta_us);
                det->exit_hold_anchor_us = t;
                det->exit_hold_anchor_seq = sample->window_seq;
                det->exit_hold_anchor_valid = true;
            } else {
                /* Gap / discontinuity: pause; re-anchor with zero credit. */
                det->exit_hold_anchor_us = t;
                det->exit_hold_anchor_seq = sample->window_seq;
                det->exit_hold_anchor_valid = true;
            }

            const int64_t hold_us = (int64_t)latched_exit_hold_ms(det) * 1000LL;
            if (det->exit_hold_elapsed_us >= hold_us) {
                note_exit_hold_peak(det);
                int64_t start = det->last_emit_us;
                det->state = DBMAP_ACTIVITY_IDLE;
                det->pending_since_us = 0;
                det->last_emit_us = t;
                det->emissions_end += 1;
                on_episode_end_cleanup(det);
                return make_emission(
                    DBMAP_ACTIVITY_EMIT_END, DBMAP_ACTIVITY_TERM_NATURAL, rms, start, t
                );
            }
        }
        return none;

    default:
        det->state = DBMAP_ACTIVITY_IDLE;
        return none;
    }
}

dbmap_activity_emission_t dbmap_activity_detector_force_end(dbmap_activity_detector_t *det)
{
    dbmap_activity_emission_t none;
    memset(&none, 0, sizeof(none));
    none.kind = DBMAP_ACTIVITY_EMIT_NONE;
    none.termination = DBMAP_ACTIVITY_TERM_NONE;

    if (det == NULL) {
        return none;
    }
    if (det->state != DBMAP_ACTIVITY_ACTIVE && det->state != DBMAP_ACTIVITY_PENDING_EXIT) {
        return none;
    }

    int64_t end_us = det->has_last_valid ? det->last_valid_us : det->last_emit_us;
    int64_t start_us = det->last_emit_us;
    float level = det->last_level_dbfs;
    det->state = DBMAP_ACTIVITY_IDLE;
    det->pending_since_us = 0;
    det->last_emit_us = end_us;
    det->emissions_end += 1;
    on_episode_end_cleanup(det);
    /* Termination reason for OTA is applied by the Observation layer. */
    return make_emission(
        DBMAP_ACTIVITY_EMIT_END, DBMAP_ACTIVITY_TERM_NONE, level, start_us, end_us
    );
}

void dbmap_activity_detector_reset_idle(dbmap_activity_detector_t *det)
{
    if (det == NULL) {
        return;
    }
    det->state = DBMAP_ACTIVITY_IDLE;
    det->pending_since_us = 0;
    on_episode_end_cleanup(det);
}
