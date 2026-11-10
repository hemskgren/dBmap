#include "proto_relative_activity.h"

#include <math.h>
#include <string.h>

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

void proto_relative_detector_init(
    proto_relative_detector_t *det,
    const proto_relative_config_t *cfg
)
{
    if (det == NULL) {
        return;
    }
    memset(det, 0, sizeof(*det));
    if (cfg != NULL) {
        det->cfg = *cfg;
    }
    det->state = DBMAP_ACTIVITY_IDLE;
    det->boot_complete = (det->cfg.abs_cfg.boot_settle_ms == 0);
    proto_pre_event_baseline_init(&det->baseline, det->cfg.stale_max_ms);
}

dbmap_activity_state_t proto_relative_detector_state(const proto_relative_detector_t *det)
{
    return det != NULL ? det->state : DBMAP_ACTIVITY_IDLE;
}

bool proto_relative_detector_relative_armed(const proto_relative_detector_t *det)
{
    return det != NULL && det->relative_armed;
}

static bool above_enter(const proto_relative_detector_t *det, float rms)
{
    return rms >= det->cfg.abs_cfg.enter_threshold_dbfs;
}

static bool relative_ready(const proto_relative_detector_t *det)
{
    return det->cfg.exit_mode == PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK &&
           det->baseline.frozen && det->baseline.reliable && det->relative_armed;
}

static bool below_relative_primary(const proto_relative_detector_t *det, float rms)
{
    return rms <= det->baseline.frozen_baseline_dbfs + det->cfg.return_margin_db;
}

static bool below_relative_secondary(const proto_relative_detector_t *det, float rms)
{
    if (det->cfg.ambient_policy != PROTO_AMBIENT_POLICY_SECONDARY_EXIT) {
        return false;
    }
    if (!det->secondary_ambient_valid) {
        return false;
    }
    return rms <= det->secondary_ambient_dbfs + det->cfg.return_margin_db;
}

static bool below_exit_active(proto_relative_detector_t *det, float rms)
{
    if (det->cfg.exit_mode == PROTO_EXIT_MODE_ABSOLUTE) {
        return rms < det->cfg.abs_cfg.exit_threshold_dbfs;
    }
    if (relative_ready(det)) {
        /* Match production v1: absolute remains available while relatively armed. */
        return below_relative_primary(det, rms) || below_relative_secondary(det, rms) ||
               (rms < det->cfg.abs_cfg.exit_threshold_dbfs);
    }
    if (det->baseline.frozen && det->baseline.reliable && !det->relative_armed &&
        below_relative_primary(det, rms)) {
        det->contrast_blocked_end = true;
    }
    return rms < det->cfg.abs_cfg.exit_threshold_dbfs;
}

static bool still_below_exit(const proto_relative_detector_t *det, float rms)
{
    if (det->cfg.exit_mode == PROTO_EXIT_MODE_ABSOLUTE) {
        return rms < det->cfg.abs_cfg.exit_threshold_dbfs;
    }
    if (relative_ready(det)) {
        const float cancel_primary = det->baseline.frozen_baseline_dbfs + det->cfg.return_margin_db +
                                     det->cfg.cancel_hysteresis_db;
        if (rms <= cancel_primary) {
            return true;
        }
        if (det->secondary_ambient_valid &&
            det->cfg.ambient_policy == PROTO_AMBIENT_POLICY_SECONDARY_EXIT) {
            const float cancel_sec = det->secondary_ambient_dbfs + det->cfg.return_margin_db +
                                     det->cfg.cancel_hysteresis_db;
            return rms <= cancel_sec;
        }
        return false;
    }
    return rms < det->cfg.abs_cfg.exit_threshold_dbfs;
}

static uint32_t exit_hold_ms(const proto_relative_detector_t *det)
{
    if (relative_ready(det)) {
        return det->cfg.relative_exit_hold_ms;
    }
    return det->cfg.abs_cfg.exit_hold_ms;
}

static void mark_end_path(proto_relative_detector_t *det, float rms)
{
    det->last_end_used_secondary = false;
    det->last_end_relative_armed = det->relative_armed;
    det->last_end_contrast_db = det->measured_contrast_db;
    if (relative_ready(det)) {
        if (below_relative_secondary(det, rms) && !below_relative_primary(det, rms)) {
            det->last_end_used_secondary = true;
            det->last_end_used_relative = true;
            det->last_end_used_fallback = false;
            det->baseline.used_fallback = false;
            return;
        }
        if (below_relative_primary(det, rms) || below_relative_secondary(det, rms)) {
            det->last_end_used_relative = true;
            det->last_end_used_fallback = false;
            det->baseline.used_fallback = false;
            if (below_relative_secondary(det, rms) && below_relative_primary(det, rms)) {
                /* prefer primary label when both match */
                det->last_end_used_secondary = false;
            } else if (below_relative_secondary(det, rms)) {
                det->last_end_used_secondary = true;
            }
            return;
        }
    }
    det->last_end_used_relative = false;
    det->last_end_used_fallback = (det->cfg.exit_mode == PROTO_EXIT_MODE_RELATIVE_WITH_FALLBACK);
    det->baseline.used_fallback = det->last_end_used_fallback;
}

static void reset_episode_tracking(proto_relative_detector_t *det)
{
    det->relative_armed = (det->cfg.contrast_method == PROTO_CONTRAST_DISABLED);
    det->event_peak_dbfs = -120.0f;
    det->arming_count = 0;
    det->measured_contrast_db = 0.0f;
    det->contrast_blocked_end = false;
    det->ambient_shift_suspected = false;
    det->secondary_ambient_valid = false;
    det->secondary_ambient_dbfs = 0.0f;
    det->amb_stable_count = 0;
    det->last_end_used_secondary = false;
    det->last_end_used_refresh = false;
    det->last_end_used_below_enter = false;
    det->reused_prior_baseline = false;
    det->recent_count = 0;
    det->recent_head = 0;
    det->episode_usable_n = 0;
    memset(det->recent_rms, 0, sizeof(det->recent_rms));
    det->below_enter_anchor_us = 0;
    det->below_enter_anchor_valid = false;
    det->below_enter_elapsed_us = 0;
}

static void try_arm_contrast(proto_relative_detector_t *det)
{
    if (det->cfg.contrast_method == PROTO_CONTRAST_DISABLED) {
        det->relative_armed = true;
        return;
    }
    if (!det->baseline.frozen || !det->baseline.reliable) {
        det->relative_armed = false;
        return;
    }
    if (det->relative_armed) {
        return;
    }

    float event_level = det->event_peak_dbfs;
    if (det->cfg.contrast_method == PROTO_CONTRAST_ARMING_MEDIAN && det->arming_count > 0) {
        event_level = proto_pre_event_baseline_median(det->arming_rms, det->arming_count);
    } else if (det->cfg.contrast_method == PROTO_CONTRAST_ARMING_MAX && det->arming_count > 0) {
        event_level = det->arming_rms[0];
        for (uint32_t i = 1; i < det->arming_count; ++i) {
            if (det->arming_rms[i] > event_level) {
                event_level = det->arming_rms[i];
            }
        }
        det->event_peak_dbfs = event_level;
    } else if (det->cfg.contrast_method == PROTO_CONTRAST_START_WINDOW) {
        event_level = det->event_peak_dbfs;
    } else if (det->cfg.contrast_method == PROTO_CONTRAST_EPISODE_PEAK) {
        event_level = det->event_peak_dbfs;
        if (det->cfg.recent_peak_windows > 0 && det->recent_count > 0) {
            uint32_t n = det->cfg.recent_peak_windows;
            if (n > 16) {
                n = 16;
            }
            if (n > det->recent_count) {
                n = det->recent_count;
            }
            event_level = -120.0f;
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t idx =
                    (det->recent_head + 16 - n + i) % 16;
                if (det->recent_rms[idx] > event_level) {
                    event_level = det->recent_rms[idx];
                }
            }
        }
    }

    det->measured_contrast_db = event_level - det->baseline.frozen_baseline_dbfs;
    if (det->measured_contrast_db >= det->cfg.min_event_contrast_db) {
        det->relative_armed = true;
    }
}

static void note_recent_rms(proto_relative_detector_t *det, float rms)
{
    det->recent_rms[det->recent_head] = rms;
    det->recent_head = (det->recent_head + 1u) % 16;
    if (det->recent_count < 16) {
        det->recent_count++;
    }
    det->episode_usable_n++;
}

static void note_event_rms(proto_relative_detector_t *det, float rms)
{
    if (det->cfg.contrast_method == PROTO_CONTRAST_DISABLED) {
        return;
    }
    /* START_WINDOW: only the START emission sample arms contrast. */
    if (det->cfg.contrast_method == PROTO_CONTRAST_START_WINDOW && det->arming_count >= 1) {
        return;
    }
    if (rms > det->event_peak_dbfs) {
        det->event_peak_dbfs = rms;
    }
    note_recent_rms(det, rms);

    if (det->cfg.contrast_method == PROTO_CONTRAST_EPISODE_PEAK) {
        /* Re-evaluate every window from peak (or recent peak window). */
        try_arm_contrast(det);
        return;
    }

    uint32_t need = det->cfg.contrast_arming_windows;
    if (need == 0) {
        need = 1;
    }
    if (need > 8) {
        need = 8;
    }
    if (det->cfg.contrast_method == PROTO_CONTRAST_START_WINDOW) {
        need = 1;
    }
    if (det->arming_count < need) {
        det->arming_rms[det->arming_count++] = rms;
    }
    try_arm_contrast(det);
}

static void update_ambient_indicator(proto_relative_detector_t *det, float rms)
{
    if (det->cfg.ambient_policy == PROTO_AMBIENT_POLICY_NONE) {
        return;
    }
    if (!det->baseline.frozen || !det->baseline.reliable) {
        return;
    }
    /* Only consider below-enter energy so loud source is not absorbed. */
    if (above_enter(det, rms)) {
        det->amb_stable_count = 0;
        return;
    }

    uint32_t need = det->cfg.ambient_stable_windows;
    if (need == 0) {
        need = 3;
    }
    if (need > 8) {
        need = 8;
    }
    if (det->amb_stable_count < need) {
        det->amb_stable_rms[det->amb_stable_count++] = rms;
    } else {
        memmove(
            &det->amb_stable_rms[0],
            &det->amb_stable_rms[1],
            sizeof(float) * (need - 1)
        );
        det->amb_stable_rms[need - 1] = rms;
        det->amb_stable_count = need;
    }
    if (det->amb_stable_count < need) {
        return;
    }

    float mn = det->amb_stable_rms[0];
    float mx = det->amb_stable_rms[0];
    for (uint32_t i = 1; i < need; ++i) {
        if (det->amb_stable_rms[i] < mn) {
            mn = det->amb_stable_rms[i];
        }
        if (det->amb_stable_rms[i] > mx) {
            mx = det->amb_stable_rms[i];
        }
    }
    const float range_lim =
        det->cfg.ambient_stable_range_db > 0.0f ? det->cfg.ambient_stable_range_db : 3.0f;
    if ((mx - mn) > range_lim) {
        return;
    }
    const float med = proto_pre_event_baseline_median(det->amb_stable_rms, need);
    const float jump_need =
        det->cfg.ambient_jump_db > 0.0f ? det->cfg.ambient_jump_db : 12.0f;
    if ((med - det->baseline.frozen_baseline_dbfs) < jump_need) {
        return;
    }

    det->ambient_shift_suspected = true;
    det->secondary_ambient_dbfs = med;
    det->secondary_ambient_valid = true;

    if (det->cfg.ambient_policy == PROTO_AMBIENT_POLICY_REFRESH_BASELINE) {
        /* Rewrite freeze so primary relative entry tracks new ambient. */
        det->baseline.frozen_baseline_dbfs = med;
        det->secondary_ambient_valid = false; /* primary path owns EXIT after refresh */
    }
}

static void on_start(proto_relative_detector_t *det, int64_t episode_start_us, float start_rms)
{
    reset_episode_tracking(det);
    (void)proto_pre_event_baseline_freeze(&det->baseline, episode_start_us);

    /* Optional reuse of prior freeze when new baseline unreliable (Investigation C). */
    if (!det->baseline.reliable && det->have_prior_baseline &&
        det->cfg.reuse_prior_baseline_max_age_ms > 0 && det->prior_end_us > 0) {
        const int64_t age_ms = (episode_start_us - det->prior_end_us) / 1000;
        if (age_ms >= 0 &&
            age_ms <= (int64_t)det->cfg.reuse_prior_baseline_max_age_ms) {
            det->baseline.frozen = true;
            det->baseline.reliable = true;
            det->baseline.frozen_baseline_dbfs = det->prior_baseline_dbfs;
            det->baseline.frozen_sample_count = det->prior_baseline_n;
            det->reused_prior_baseline = true;
        }
    }

    if (det->cfg.contrast_method == PROTO_CONTRAST_DISABLED) {
        det->relative_armed = det->baseline.reliable;
    } else {
        det->relative_armed = false;
        note_event_rms(det, start_rms);
        if (det->cfg.contrast_method == PROTO_CONTRAST_START_WINDOW) {
            try_arm_contrast(det);
        }
    }
}

static void on_end_clear(proto_relative_detector_t *det, int64_t end_us)
{
    if (det->baseline.frozen && det->baseline.reliable) {
        det->have_prior_baseline = true;
        det->prior_baseline_dbfs = det->baseline.frozen_baseline_dbfs;
        det->prior_baseline_n = det->baseline.frozen_sample_count;
        det->prior_end_us = end_us;
    }
    proto_pre_event_baseline_clear(&det->baseline);
    det->relative_armed = false;
    det->secondary_ambient_valid = false;
    det->amb_stable_count = 0;
}

static dbmap_activity_emission_t force_end_after_invalid(proto_relative_detector_t *det)
{
    int64_t end_us = det->has_last_valid ? det->last_valid_us : det->last_emit_us;
    int64_t start_us = det->last_emit_us;
    float level = det->last_level_dbfs;
    det->state = DBMAP_ACTIVITY_IDLE;
    det->pending_since_us = 0;
    det->last_emit_us = end_us;
    det->emissions_end += 1;
    det->forced_ends_invalid += 1;
    det->last_end_used_relative = false;
    det->last_end_used_fallback = false;
    det->last_end_used_secondary = false;
    on_end_clear(det, end_us);
    return make_emission(
        DBMAP_ACTIVITY_EMIT_END, DBMAP_ACTIVITY_TERM_INVALID_HOLD, level, start_us, end_us
    );
}

static dbmap_activity_emission_t maybe_periodic(
    proto_relative_detector_t *det,
    float rms,
    int64_t t
)
{
    int64_t due =
        det->last_emit_us + ((int64_t)det->cfg.abs_cfg.observation_interval_ms * 1000LL);
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

dbmap_activity_emission_t proto_relative_detector_feed(
    proto_relative_detector_t *det,
    const dbmap_activity_sample_t *sample
)
{
    dbmap_activity_emission_t none;
    memset(&none, 0, sizeof(none));
    none.kind = DBMAP_ACTIVITY_EMIT_NONE;

    if (det == NULL || sample == NULL || !dbmap_activity_config_valid(&det->cfg.abs_cfg)) {
        return none;
    }
    if (!sample->has_measurement) {
        return none;
    }

    if (det->has_last_window && sample->window_seq == det->last_window_seq) {
        return none;
    }
    det->has_last_window = true;
    det->last_window_seq = sample->window_seq;

    if (!det->boot_complete) {
        if (sample->ms_since_i2s_enable < det->cfg.abs_cfg.boot_settle_ms) {
            return none;
        }
        det->boot_complete = true;
    }

    if (!dbmap_activity_sample_usable(sample)) {
        if (det->state == DBMAP_ACTIVITY_PENDING_ENTER) {
            if (det->cfg.abs_cfg.max_invalid_hold_ms > 0 && det->has_last_valid &&
                (sample->measured_at_us - det->last_valid_us) >=
                    ((int64_t)det->cfg.abs_cfg.max_invalid_hold_ms * 1000LL)) {
                det->state = DBMAP_ACTIVITY_IDLE;
                det->pending_since_us = 0;
            } else if (det->cfg.abs_cfg.max_invalid_hold_ms > 0 && !det->has_last_valid &&
                       (sample->measured_at_us - det->pending_since_us) >=
                           ((int64_t)det->cfg.abs_cfg.max_invalid_hold_ms * 1000LL)) {
                det->state = DBMAP_ACTIVITY_IDLE;
                det->pending_since_us = 0;
            }
            return none;
        }
        if ((det->state == DBMAP_ACTIVITY_ACTIVE || det->state == DBMAP_ACTIVITY_PENDING_EXIT) &&
            det->cfg.abs_cfg.max_invalid_hold_ms > 0 && det->has_last_valid &&
            (sample->measured_at_us - det->last_valid_us) >=
                ((int64_t)det->cfg.abs_cfg.max_invalid_hold_ms * 1000LL)) {
            return force_end_after_invalid(det);
        }
        /* Invalid must not advance ambient stability or exit hold. */
        return none;
    }

    const float rms = sample->rms_dbfs;
    const int64_t t = sample->measured_at_us;
    const uint32_t window_ms = sample->window_ms > 0 ? sample->window_ms : 1;
    const int64_t window_start = t - ((int64_t)window_ms * 1000LL);
    det->last_level_dbfs = rms;
    det->last_valid_us = t;
    det->has_last_valid = true;

    if (det->state == DBMAP_ACTIVITY_IDLE && !above_enter(det, rms)) {
        proto_pre_event_baseline_push(&det->baseline, rms, t);
    }

    switch (det->state) {
    case DBMAP_ACTIVITY_IDLE:
        if (above_enter(det, rms)) {
            det->pending_since_us = window_start;
            det->episode_start_us = window_start;
            if ((t - window_start) >= ((int64_t)det->cfg.abs_cfg.min_activity_ms * 1000LL)) {
                det->state = DBMAP_ACTIVITY_ACTIVE;
                det->last_emit_us = t;
                det->emissions_start += 1;
                on_start(det, det->episode_start_us, rms);
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
        if ((t - det->pending_since_us) >= ((int64_t)det->cfg.abs_cfg.min_activity_ms * 1000LL)) {
            det->state = DBMAP_ACTIVITY_ACTIVE;
            det->episode_start_us = det->pending_since_us;
            det->last_emit_us = t;
            det->emissions_start += 1;
            on_start(det, det->episode_start_us, rms);
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
        note_event_rms(det, rms);
        update_ambient_indicator(det, rms);
        /* Alt3b: continuous below-enter dwell END (unarmed stuck band). */
        if (det->cfg.below_enter_end_hold_ms > 0 && !above_enter(det, rms)) {
            if (!det->below_enter_anchor_valid) {
                det->below_enter_anchor_us = t;
                det->below_enter_anchor_valid = true;
                det->below_enter_elapsed_us = 0;
            } else {
                const int64_t d = t - det->below_enter_anchor_us;
                if (d > 0 && d <= ((int64_t)window_ms * 1000 * 3 / 2)) {
                    det->below_enter_elapsed_us += d;
                }
                det->below_enter_anchor_us = t;
            }
            if (det->below_enter_elapsed_us >=
                ((int64_t)det->cfg.below_enter_end_hold_ms * 1000LL)) {
                int64_t start = det->last_emit_us;
                mark_end_path(det, rms);
                det->last_end_used_below_enter = true;
                det->state = DBMAP_ACTIVITY_IDLE;
                det->pending_since_us = 0;
                det->last_emit_us = t;
                det->emissions_end += 1;
                on_end_clear(det, t);
                return make_emission(
                    DBMAP_ACTIVITY_EMIT_END, DBMAP_ACTIVITY_TERM_NATURAL, rms, start, t
                );
            }
        } else {
            det->below_enter_anchor_valid = false;
            det->below_enter_elapsed_us = 0;
        }
        if (below_exit_active(det, rms)) {
            det->state = DBMAP_ACTIVITY_PENDING_EXIT;
            det->pending_since_us = t;
        }
        return maybe_periodic(det, rms, t);

    case DBMAP_ACTIVITY_PENDING_EXIT:
        note_event_rms(det, rms);
        update_ambient_indicator(det, rms);
        det->below_enter_anchor_valid = false;
        det->below_enter_elapsed_us = 0;
        if (!still_below_exit(det, rms)) {
            det->state = DBMAP_ACTIVITY_ACTIVE;
            det->pending_since_us = 0;
            return maybe_periodic(det, rms, t);
        }
        if ((t - det->pending_since_us) >= ((int64_t)exit_hold_ms(det) * 1000LL)) {
            int64_t start = det->last_emit_us;
            mark_end_path(det, rms);
            det->state = DBMAP_ACTIVITY_IDLE;
            det->pending_since_us = 0;
            det->last_emit_us = t;
            det->emissions_end += 1;
            on_end_clear(det, t);
            return make_emission(
                DBMAP_ACTIVITY_EMIT_END, DBMAP_ACTIVITY_TERM_NATURAL, rms, start, t
            );
        }
        return none;

    default:
        det->state = DBMAP_ACTIVITY_IDLE;
        return none;
    }
}
