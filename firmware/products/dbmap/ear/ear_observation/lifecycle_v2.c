#include "dbmap_lifecycle_v2.h"

#include <math.h>
#include <string.h>

/*
 * Signature score normalization (explainable, NOT a probability):
 *
 *   tonality      = clamp(1 - flatness, 0, 1)
 *   dom_stability = has_Δf ? clamp(1 - |Δf| / 200 Hz, 0, 1) : 0.5
 *   low_flux      = has_flux ? clamp(1 - flux / 2.0, 0, 1) : 0.5
 *   low_crest     = clamp(1 - (crest_db - 3) / 15, 0, 1)
 *
 *   instant = 0.35*tonality + 0.25*dom_stability + 0.20*low_flux + 0.20*low_crest
 *
 * High instant ≈ sustained tonal / stable scene (e.g. continuous whistle/motor).
 * Low instant ≈ impulsive / noisy (e.g. keyboard). Missing features → no hist push.
 *
 * persistent = arithmetic mean of the last persistence_windows valid instants.
 */

#define LC_V2_DELTA_REF_HZ 200.0f
#define LC_V2_FLUX_REF 2.0f
#define LC_V2_CREST_FLOOR_DB 3.0f
#define LC_V2_CREST_SPAN_DB 15.0f

static float clampf(float x, float lo, float hi)
{
    if (x < lo) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}

static void clear_exit_hold(dbmap_lifecycle_v2_t *lc)
{
    lc->exit_hold_elapsed_us = 0;
    lc->exit_hold_anchor_us = 0;
    lc->exit_hold_anchor_seq = 0;
    lc->exit_hold_anchor_valid = false;
}

static void begin_exit_hold(dbmap_lifecycle_v2_t *lc, int64_t t, uint32_t seq)
{
    lc->exit_hold_elapsed_us = 0;
    lc->exit_hold_anchor_us = t;
    lc->exit_hold_anchor_seq = seq;
    lc->exit_hold_anchor_valid = true;
}

static void add_exit_hold(dbmap_lifecycle_v2_t *lc, int64_t delta_us)
{
    if (delta_us <= 0) {
        return;
    }
    if (lc->exit_hold_elapsed_us > INT64_MAX - delta_us) {
        lc->exit_hold_elapsed_us = INT64_MAX;
    } else {
        lc->exit_hold_elapsed_us += delta_us;
    }
}

static bool seq_is_consecutive(uint32_t prev, uint32_t cur)
{
    return prev != UINT32_MAX && cur == prev + 1u;
}

static dbmap_activity_emission_t make_emission(
    dbmap_activity_emit_kind_t kind,
    dbmap_activity_termination_t term,
    float level,
    int64_t start_us,
    int64_t end_us
)
{
    dbmap_activity_emission_t e;
    memset(&e, 0, sizeof(e));
    e.kind = kind;
    e.termination = term;
    e.signal_level_dbfs = level;
    e.interval_start_mono_us = start_us;
    e.interval_end_mono_us = end_us;
    e.duration_ms = (double)(end_us - start_us) / 1000.0;
    if (e.duration_ms < 0.0) {
        e.duration_ms = 0.0;
    }
    return e;
}

static dbmap_activity_emission_t none_emission(void)
{
    return make_emission(
        DBMAP_ACTIVITY_EMIT_NONE, DBMAP_ACTIVITY_TERM_NONE, 0.0f, 0, 0
    );
}

void dbmap_lifecycle_v2_config_set_defaults(dbmap_lifecycle_v2_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->gate_enter_dbfs = -45.0f;
    cfg->quiet_fallback_dbfs = -55.0f;
    cfg->min_activity_ms = 2000;
    cfg->boot_settle_ms = 5000;
    cfg->observation_interval_ms = 5000;
    cfg->max_invalid_hold_ms = 30000;
    cfg->score_enter = 0.40f;
    cfg->score_exit = 0.32f;
    cfg->persistence_windows = 3;
    cfg->signature_exit_hold_ms = 2000;
    cfg->quiet_exit_hold_ms = 1000;
    cfg->max_episode_ms = 120000; /* 2 min hard cap */
    cfg->pre_trigger_ms = 2000;
}

bool dbmap_lifecycle_v2_config_valid(const dbmap_lifecycle_v2_config_t *cfg)
{
    if (cfg == NULL) {
        return false;
    }
    if (!isfinite((double)cfg->gate_enter_dbfs) || !isfinite((double)cfg->quiet_fallback_dbfs)) {
        return false;
    }
    if (cfg->gate_enter_dbfs > 0.0f || cfg->quiet_fallback_dbfs > 0.0f) {
        return false;
    }
    if (cfg->gate_enter_dbfs <= cfg->quiet_fallback_dbfs) {
        return false;
    }
    if (cfg->min_activity_ms == 0 || cfg->observation_interval_ms == 0) {
        return false;
    }
    if (cfg->persistence_windows == 0
        || cfg->persistence_windows > DBMAP_LC_V2_SCORE_HIST_CAP) {
        return false;
    }
    if (!isfinite((double)cfg->score_enter) || !isfinite((double)cfg->score_exit)) {
        return false;
    }
    if (cfg->score_enter < 0.0f || cfg->score_enter > 1.0f
        || cfg->score_exit < 0.0f || cfg->score_exit > 1.0f) {
        return false;
    }
    if (cfg->score_enter < cfg->score_exit) {
        return false;
    }
    if (cfg->signature_exit_hold_ms == 0 || cfg->quiet_exit_hold_ms == 0) {
        return false;
    }
    if (cfg->max_episode_ms == 0) {
        return false;
    }
    if (cfg->pre_trigger_ms < 1000 || cfg->pre_trigger_ms > 3000) {
        return false;
    }
    return true;
}

void dbmap_lifecycle_v2_init(
    dbmap_lifecycle_v2_t *lc,
    const dbmap_lifecycle_v2_config_t *cfg
)
{
    if (lc == NULL) {
        return;
    }
    memset(lc, 0, sizeof(*lc));
    if (cfg != NULL) {
        lc->cfg = *cfg;
    } else {
        dbmap_lifecycle_v2_config_set_defaults(&lc->cfg);
    }
    lc->state = DBMAP_LC_V2_IDLE;
}

dbmap_lifecycle_v2_score_t dbmap_lifecycle_v2_score_window(
    const dbmap_lifecycle_v2_window_t *win
)
{
    dbmap_lifecycle_v2_score_t s;
    memset(&s, 0, sizeof(s));
    if (win == NULL || !win->features_valid) {
        return s;
    }
    s.features_valid = true;

    const float flat = win->has_flatness ? win->flatness : 1.0f;
    s.tonality = clampf(1.0f - flat, 0.0f, 1.0f);

    if (win->has_dominant_delta) {
        float ad = win->dominant_delta_hz;
        if (ad < 0.0f) {
            ad = -ad;
        }
        s.dom_stability = clampf(1.0f - ad / LC_V2_DELTA_REF_HZ, 0.0f, 1.0f);
    } else {
        s.dom_stability = 0.5f;
    }

    if (win->has_spectral_flux) {
        s.low_flux = clampf(1.0f - win->spectral_flux / LC_V2_FLUX_REF, 0.0f, 1.0f);
    } else {
        s.low_flux = 0.5f;
    }

    s.low_crest = clampf(
        1.0f - (win->crest_db - LC_V2_CREST_FLOOR_DB) / LC_V2_CREST_SPAN_DB,
        0.0f,
        1.0f
    );

    s.instant = 0.35f * s.tonality + 0.25f * s.dom_stability + 0.20f * s.low_flux
                + 0.20f * s.low_crest;
    s.instant = clampf(s.instant, 0.0f, 1.0f);
    return s;
}

static void score_hist_push(dbmap_lifecycle_v2_t *lc, float instant)
{
    lc->score_hist[lc->score_hist_head] = instant;
    lc->score_hist_head = (lc->score_hist_head + 1u) % DBMAP_LC_V2_SCORE_HIST_CAP;
    if (lc->score_hist_count < DBMAP_LC_V2_SCORE_HIST_CAP) {
        lc->score_hist_count++;
    }
}

static float score_hist_mean(const dbmap_lifecycle_v2_t *lc, uint32_t n)
{
    if (lc->score_hist_count == 0 || n == 0) {
        return 0.0f;
    }
    if (n > lc->score_hist_count) {
        n = lc->score_hist_count;
    }
    double sum = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t idx =
            (lc->score_hist_head + DBMAP_LC_V2_SCORE_HIST_CAP - 1u - i)
            % DBMAP_LC_V2_SCORE_HIST_CAP;
        sum += (double)lc->score_hist[idx];
    }
    return (float)(sum / (double)n);
}

static void clear_score_hist(dbmap_lifecycle_v2_t *lc)
{
    lc->score_hist_count = 0;
    lc->score_hist_head = 0;
    memset(lc->score_hist, 0, sizeof(lc->score_hist));
    memset(&lc->last_score, 0, sizeof(lc->last_score));
}

static bool above_gate(const dbmap_lifecycle_v2_t *lc, float rms)
{
    return rms >= lc->cfg.gate_enter_dbfs;
}

static bool below_quiet(const dbmap_lifecycle_v2_t *lc, float rms)
{
    return rms < lc->cfg.quiet_fallback_dbfs;
}

static uint32_t latched_hold_ms(const dbmap_lifecycle_v2_t *lc)
{
    if (lc->exit_latch == DBMAP_LC_V2_EXIT_QUIET) {
        return lc->cfg.quiet_exit_hold_ms;
    }
    return lc->cfg.signature_exit_hold_ms;
}

static bool should_enter_pending_exit(const dbmap_lifecycle_v2_t *lc)
{
    if (below_quiet(lc, lc->last_level_dbfs)) {
        return true;
    }
    if (lc->last_score.features_valid
        && lc->score_hist_count >= lc->cfg.persistence_windows
        && lc->last_score.persistent < lc->cfg.score_exit) {
        return true;
    }
    return false;
}

static dbmap_lifecycle_v2_exit_latch_t choose_exit_latch(const dbmap_lifecycle_v2_t *lc)
{
    if (below_quiet(lc, lc->last_level_dbfs)) {
        return DBMAP_LC_V2_EXIT_QUIET;
    }
    return DBMAP_LC_V2_EXIT_SIGNATURE;
}

static bool should_stay_pending_exit(const dbmap_lifecycle_v2_t *lc)
{
    if (lc->exit_latch == DBMAP_LC_V2_EXIT_QUIET) {
        return below_quiet(lc, lc->last_level_dbfs);
    }
    /* Signature latch: stay while persistent score remains below enter (hysteresis). */
    if (!lc->last_score.features_valid) {
        /* Missing features pause hold credit (handled by caller) but stay latched. */
        return true;
    }
    return lc->last_score.persistent < lc->cfg.score_enter;
}

static dbmap_activity_emission_t maybe_periodic(
    dbmap_lifecycle_v2_t *lc,
    float rms,
    int64_t t
)
{
    const int64_t due =
        lc->last_emit_us + ((int64_t)lc->cfg.observation_interval_ms * 1000LL);
    if (t >= due) {
        const int64_t start = lc->last_emit_us;
        lc->last_emit_us = t;
        lc->emissions_periodic += 1;
        return make_emission(
            DBMAP_ACTIVITY_EMIT_PERIODIC, DBMAP_ACTIVITY_TERM_NONE, rms, start, t
        );
    }
    return none_emission();
}

static dbmap_activity_emission_t emit_end(
    dbmap_lifecycle_v2_t *lc,
    dbmap_activity_termination_t term,
    float level,
    int64_t end_us
)
{
    if (lc->end_emitted) {
        return none_emission();
    }
    const int64_t start = lc->last_emit_us;
    lc->state = DBMAP_LC_V2_IDLE;
    lc->pending_since_us = 0;
    lc->last_emit_us = end_us;
    lc->end_emitted = true;
    lc->emissions_end += 1;
    lc->exit_latch = DBMAP_LC_V2_EXIT_NONE;
    clear_exit_hold(lc);
    clear_score_hist(lc);
    return make_emission(DBMAP_ACTIVITY_EMIT_END, term, level, start, end_us);
}

static dbmap_activity_emission_t force_end_invalid(dbmap_lifecycle_v2_t *lc)
{
    const int64_t end_us = lc->has_last_valid ? lc->last_valid_us : lc->last_emit_us;
    const float level = lc->last_level_dbfs;
    lc->forced_ends_invalid += 1;
    return emit_end(lc, DBMAP_ACTIVITY_TERM_INVALID_HOLD, level, end_us);
}

static bool credit_exit_hold(
    dbmap_lifecycle_v2_t *lc,
    const dbmap_lifecycle_v2_window_t *win,
    int64_t t,
    uint32_t window_ms,
    bool features_ok_for_credit
)
{
    if (!features_ok_for_credit && lc->exit_latch == DBMAP_LC_V2_EXIT_SIGNATURE) {
        /* Invalid/missing features: pause signature hold (re-anchor, no credit). */
        lc->exit_hold_anchor_us = t;
        lc->exit_hold_anchor_seq = win->sample.window_seq;
        lc->exit_hold_anchor_valid = true;
        return false;
    }

    const int64_t contig_max_us = (int64_t)window_ms * 1000 * 3 / 2;
    const bool seq_ok = lc->exit_hold_anchor_valid
                        && seq_is_consecutive(lc->exit_hold_anchor_seq, win->sample.window_seq);
    const bool time_ok = lc->exit_hold_anchor_valid && t >= lc->exit_hold_anchor_us;
    const int64_t delta_us = time_ok ? (t - lc->exit_hold_anchor_us) : (int64_t)0;
    const bool continuous = seq_ok && time_ok && delta_us > 0 && delta_us <= contig_max_us;

    if (continuous) {
        add_exit_hold(lc, delta_us);
        lc->exit_hold_anchor_us = t;
        lc->exit_hold_anchor_seq = win->sample.window_seq;
        lc->exit_hold_anchor_valid = true;
    } else {
        lc->exit_hold_anchor_us = t;
        lc->exit_hold_anchor_seq = win->sample.window_seq;
        lc->exit_hold_anchor_valid = true;
    }
    return lc->exit_hold_elapsed_us >= ((int64_t)latched_hold_ms(lc) * 1000LL);
}

dbmap_activity_emission_t dbmap_lifecycle_v2_feed(
    dbmap_lifecycle_v2_t *lc,
    const dbmap_lifecycle_v2_window_t *win
)
{
    if (lc == NULL || win == NULL || !dbmap_lifecycle_v2_config_valid(&lc->cfg)) {
        return none_emission();
    }
    const dbmap_activity_sample_t *sample = &win->sample;
    if (!sample->has_measurement) {
        return none_emission();
    }
    if (lc->has_last_window && sample->window_seq == lc->last_window_seq) {
        return none_emission();
    }

    if (lc->has_last_window && lc->state == DBMAP_LC_V2_PENDING_EXIT) {
        const uint32_t last = lc->last_window_seq;
        const uint32_t cur = sample->window_seq;
        if (cur < last || (last != UINT32_MAX && cur > last + 1u)) {
            lc->exit_hold_anchor_valid = false;
        }
    }

    lc->has_last_window = true;
    lc->last_window_seq = sample->window_seq;
    lc->windows_seen += 1;

    if (!lc->boot_complete) {
        if (sample->ms_since_i2s_enable < lc->cfg.boot_settle_ms) {
            lc->windows_boot_settle += 1;
            return none_emission();
        }
        lc->boot_complete = true;
    }

    if (!dbmap_activity_sample_usable(sample)) {
        lc->windows_invalid += 1;
        if (lc->state == DBMAP_LC_V2_PENDING_EXIT) {
            lc->exit_hold_anchor_valid = false;
        }
        if (lc->state == DBMAP_LC_V2_PENDING_ENTER) {
            if (lc->cfg.max_invalid_hold_ms > 0 && lc->has_last_valid
                && (sample->measured_at_us - lc->last_valid_us)
                       >= ((int64_t)lc->cfg.max_invalid_hold_ms * 1000LL)) {
                lc->state = DBMAP_LC_V2_IDLE;
                lc->pending_since_us = 0;
            }
            return none_emission();
        }
        if ((lc->state == DBMAP_LC_V2_ACTIVE || lc->state == DBMAP_LC_V2_PENDING_EXIT)
            && lc->cfg.max_invalid_hold_ms > 0 && lc->has_last_valid
            && (sample->measured_at_us - lc->last_valid_us)
                   >= ((int64_t)lc->cfg.max_invalid_hold_ms * 1000LL)) {
            return force_end_invalid(lc);
        }
        return none_emission();
    }

    const float rms = sample->rms_dbfs;
    const int64_t t = sample->measured_at_us;
    const uint32_t window_ms = sample->window_ms > 0 ? sample->window_ms : 1;
    const int64_t window_start = t - ((int64_t)window_ms * 1000LL);
    lc->last_level_dbfs = rms;
    lc->last_valid_us = t;
    lc->has_last_valid = true;

    /* Signature score update (valid features only). */
    dbmap_lifecycle_v2_score_t scored = dbmap_lifecycle_v2_score_window(win);
    if (scored.features_valid) {
        score_hist_push(lc, scored.instant);
        scored.persistent = score_hist_mean(lc, lc->cfg.persistence_windows);
    } else {
        scored.persistent = lc->last_score.persistent;
    }
    lc->last_score = scored;

    /* Max episode age (ACTIVE / PENDING_EXIT). */
    if ((lc->state == DBMAP_LC_V2_ACTIVE || lc->state == DBMAP_LC_V2_PENDING_EXIT)
        && !lc->end_emitted
        && (t - lc->episode_start_us) >= ((int64_t)lc->cfg.max_episode_ms * 1000LL)) {
        lc->ends_max_age += 1;
        return emit_end(lc, DBMAP_ACTIVITY_TERM_NATURAL, rms, t);
    }

    switch (lc->state) {
    case DBMAP_LC_V2_IDLE:
        lc->end_emitted = false;
        if (above_gate(lc, rms)) {
            lc->pending_since_us = window_start;
            lc->episode_start_us = window_start;
            if ((t - window_start) >= ((int64_t)lc->cfg.min_activity_ms * 1000LL)) {
                lc->state = DBMAP_LC_V2_ACTIVE;
                lc->last_emit_us = t;
                lc->emissions_start += 1;
                lc->end_emitted = false;
                {
                    int64_t start =
                        lc->episode_start_us - ((int64_t)lc->cfg.pre_trigger_ms * 1000LL);
                    if (start < 0) {
                        start = 0;
                    }
                    return make_emission(
                        DBMAP_ACTIVITY_EMIT_START,
                        DBMAP_ACTIVITY_TERM_NONE,
                        rms,
                        start,
                        t
                    );
                }
            }
            lc->state = DBMAP_LC_V2_PENDING_ENTER;
        }
        return none_emission();

    case DBMAP_LC_V2_PENDING_ENTER:
        if (!above_gate(lc, rms)) {
            lc->state = DBMAP_LC_V2_IDLE;
            lc->pending_since_us = 0;
            return none_emission();
        }
        if ((t - lc->pending_since_us) >= ((int64_t)lc->cfg.min_activity_ms * 1000LL)) {
            lc->state = DBMAP_LC_V2_ACTIVE;
            lc->episode_start_us = lc->pending_since_us;
            lc->last_emit_us = t;
            lc->emissions_start += 1;
            lc->end_emitted = false;
            {
                int64_t start =
                    lc->episode_start_us - ((int64_t)lc->cfg.pre_trigger_ms * 1000LL);
                if (start < 0) {
                    start = 0;
                }
                return make_emission(
                    DBMAP_ACTIVITY_EMIT_START, DBMAP_ACTIVITY_TERM_NONE, rms, start, t
                );
            }
        }
        return none_emission();

    case DBMAP_LC_V2_ACTIVE:
        if (should_enter_pending_exit(lc)) {
            lc->exit_latch = choose_exit_latch(lc);
            lc->state = DBMAP_LC_V2_PENDING_EXIT;
            begin_exit_hold(lc, t, sample->window_seq);
            /* Isolated spikes: pending exit starts; PERIODIC still allowed this tick? */
            return maybe_periodic(lc, rms, t);
        }
        return maybe_periodic(lc, rms, t);

    case DBMAP_LC_V2_PENDING_EXIT:
        if (!should_stay_pending_exit(lc)) {
            /* Transient extension blocked unless score recovers to score_enter. */
            lc->state = DBMAP_LC_V2_ACTIVE;
            lc->exit_latch = DBMAP_LC_V2_EXIT_NONE;
            clear_exit_hold(lc);
            return maybe_periodic(lc, rms, t);
        }
        {
            const bool credit_ok =
                (lc->exit_latch == DBMAP_LC_V2_EXIT_QUIET) || scored.features_valid;
            if (credit_exit_hold(lc, win, t, window_ms, credit_ok)) {
                if (lc->exit_latch == DBMAP_LC_V2_EXIT_QUIET) {
                    lc->ends_quiet += 1;
                } else {
                    lc->ends_signature += 1;
                }
                return emit_end(lc, DBMAP_ACTIVITY_TERM_NATURAL, rms, t);
            }
        }
        /* No PERIODIC while exiting — matches v1 PENDING_EXIT behavior. */
        return none_emission();

    default:
        lc->state = DBMAP_LC_V2_IDLE;
        return none_emission();
    }
}

dbmap_lifecycle_v2_state_t dbmap_lifecycle_v2_state(const dbmap_lifecycle_v2_t *lc)
{
    return lc != NULL ? lc->state : DBMAP_LC_V2_IDLE;
}

dbmap_activity_emission_t dbmap_lifecycle_v2_force_end(dbmap_lifecycle_v2_t *lc)
{
    if (lc == NULL) {
        return none_emission();
    }
    if (lc->state != DBMAP_LC_V2_ACTIVE && lc->state != DBMAP_LC_V2_PENDING_EXIT) {
        return none_emission();
    }
    const int64_t end_us = lc->has_last_valid ? lc->last_valid_us : lc->last_emit_us;
    return emit_end(lc, DBMAP_ACTIVITY_TERM_NONE, lc->last_level_dbfs, end_us);
}

void dbmap_lifecycle_v2_reset_idle(dbmap_lifecycle_v2_t *lc)
{
    if (lc == NULL) {
        return;
    }
    lc->state = DBMAP_LC_V2_IDLE;
    lc->pending_since_us = 0;
    lc->exit_latch = DBMAP_LC_V2_EXIT_NONE;
    lc->end_emitted = false;
    clear_exit_hold(lc);
    clear_score_hist(lc);
}

dbmap_activity_state_t dbmap_lifecycle_v2_as_activity_state(dbmap_lifecycle_v2_state_t state)
{
    switch (state) {
    case DBMAP_LC_V2_PENDING_ENTER:
        return DBMAP_ACTIVITY_PENDING_ENTER;
    case DBMAP_LC_V2_ACTIVE:
        return DBMAP_ACTIVITY_ACTIVE;
    case DBMAP_LC_V2_PENDING_EXIT:
        return DBMAP_ACTIVITY_PENDING_EXIT;
    case DBMAP_LC_V2_IDLE:
    default:
        return DBMAP_ACTIVITY_IDLE;
    }
}
