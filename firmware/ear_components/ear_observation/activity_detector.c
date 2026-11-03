#include "dbmap_activity_detector.h"

#include <math.h>
#include <string.h>

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
    return true;
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

static bool below_exit(const dbmap_activity_detector_t *det, float rms)
{
    return rms < det->cfg.exit_threshold_dbfs;
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
    return make_emission(
        DBMAP_ACTIVITY_EMIT_END, DBMAP_ACTIVITY_TERM_INVALID_HOLD, level, start_us, end_us
    );
}

dbmap_activity_state_t dbmap_activity_detector_state(const dbmap_activity_detector_t *det)
{
    return det != NULL ? det->state : DBMAP_ACTIVITY_IDLE;
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
         * - do not advance exit_hold
         * - do not emit periodic
         * - do not drop to IDLE as "quiet"
         *
         * Timing: wall/mono clocks still advance; exit and periodic deadlines
         * are frozen until a usable sample arrives. After max_invalid_hold_ms
         * without a usable sample while in an episode, force END ending at the
         * last usable measurement (documented finite-episode policy).
         */
        if (det->state == DBMAP_ACTIVITY_PENDING_ENTER) {
            /* Never emit START without sustained usable activity. */
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

    switch (det->state) {
    case DBMAP_ACTIVITY_IDLE:
        if (above_enter(det, rms)) {
            det->pending_since_us = window_start;
            det->episode_start_us = window_start;
            if ((t - window_start) >= ((int64_t)det->cfg.min_activity_ms * 1000LL)) {
                det->state = DBMAP_ACTIVITY_ACTIVE;
                det->last_emit_us = t;
                det->emissions_start += 1;
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
        if (below_exit(det, rms)) {
            det->state = DBMAP_ACTIVITY_PENDING_EXIT;
            det->pending_since_us = t;
        } else {
            int64_t due = det->last_emit_us + ((int64_t)det->cfg.observation_interval_ms * 1000LL);
            if (t >= due) {
                int64_t start = det->last_emit_us;
                det->last_emit_us = t;
                det->emissions_periodic += 1;
                return make_emission(
                    DBMAP_ACTIVITY_EMIT_PERIODIC, DBMAP_ACTIVITY_TERM_NONE, rms, start, t
                );
            }
            return none;
        }
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
        }
        return none;

    case DBMAP_ACTIVITY_PENDING_EXIT:
        if (!below_exit(det, rms)) {
            det->state = DBMAP_ACTIVITY_ACTIVE;
            det->pending_since_us = 0;
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
            }
            return none;
        }
        if ((t - det->pending_since_us) >= ((int64_t)det->cfg.exit_hold_ms * 1000LL)) {
            int64_t start = det->last_emit_us;
            det->state = DBMAP_ACTIVITY_IDLE;
            det->pending_since_us = 0;
            det->last_emit_us = t;
            det->emissions_end += 1;
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
}
