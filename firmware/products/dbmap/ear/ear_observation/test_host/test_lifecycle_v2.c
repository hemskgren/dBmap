/**
 * Host unit tests for experimental Observation lifecycle v2.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     firmware/ear_components/ear_observation/lifecycle_v2.c \
 *     firmware/ear_components/ear_observation/activity_detector.c \
 *     firmware/ear_components/ear_observation/test_host/test_lifecycle_v2.c \
 *     -lm -o /tmp/test_dbmap_lifecycle_v2 && /tmp/test_dbmap_lifecycle_v2
 */
#include "dbmap_lifecycle_v2.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static dbmap_lifecycle_v2_config_t cfg_fast(void)
{
    dbmap_lifecycle_v2_config_t cfg;
    dbmap_lifecycle_v2_config_set_defaults(&cfg);
    cfg.boot_settle_ms = 0;
    cfg.min_activity_ms = 2000;
    cfg.observation_interval_ms = 5000;
    cfg.pre_trigger_ms = 2000;
    cfg.max_episode_ms = 15000;
    cfg.signature_exit_hold_ms = 2000;
    cfg.quiet_exit_hold_ms = 1000;
    cfg.persistence_windows = 3;
    cfg.score_enter = 0.40f;
    cfg.score_exit = 0.32f;
    cfg.max_invalid_hold_ms = 5000;
    return cfg;
}

static dbmap_lifecycle_v2_window_t win_base(
    uint32_t seq,
    int64_t t_us,
    float rms,
    float crest,
    float flatness,
    float flux,
    float dom_delta,
    bool features_valid
)
{
    dbmap_lifecycle_v2_window_t w;
    memset(&w, 0, sizeof(w));
    w.sample.has_measurement = true;
    w.sample.rms_dbfs = rms;
    w.sample.window_ms = 1000;
    w.sample.measured_at_us = t_us;
    w.sample.window_seq = seq;
    w.sample.ms_since_i2s_enable = 10000;
    w.sample.transport_status = "ok";
    w.sample.microphone_status = "signal_present";
    w.features_valid = features_valid;
    w.crest_db = crest;
    w.has_flatness = features_valid;
    w.flatness = flatness;
    w.has_spectral_flux = features_valid;
    w.spectral_flux = flux;
    w.has_dominant = features_valid;
    w.dominant_hz = 400.0f;
    w.has_dominant_delta = features_valid && seq > 1;
    w.dominant_delta_hz = dom_delta;
    return w;
}

static dbmap_lifecycle_v2_window_t win_tonal(uint32_t seq, int64_t t_us, float rms)
{
    return win_base(seq, t_us, rms, 4.0f, 0.08f, 0.15f, 5.0f, true);
}

static dbmap_lifecycle_v2_window_t win_keyboard(uint32_t seq, int64_t t_us, float rms)
{
    return win_base(seq, t_us, rms, 16.0f, 0.55f, 1.8f, 180.0f, true);
}

static dbmap_activity_emission_t feed_w(
    dbmap_lifecycle_v2_t *lc,
    dbmap_lifecycle_v2_window_t w
)
{
    return dbmap_lifecycle_v2_feed(lc, &w);
}

static dbmap_lifecycle_v2_score_t score_w(dbmap_lifecycle_v2_window_t w)
{
    return dbmap_lifecycle_v2_score_window(&w);
}


static void test_config_defaults_valid(void)
{
    dbmap_lifecycle_v2_config_t cfg;
    dbmap_lifecycle_v2_config_set_defaults(&cfg);
    EXPECT(dbmap_lifecycle_v2_config_valid(&cfg));
    cfg.pre_trigger_ms = 500;
    EXPECT(!dbmap_lifecycle_v2_config_valid(&cfg));
}

static void test_score_tonal_vs_keyboard(void)
{
    dbmap_lifecycle_v2_score_t st =
        score_w(win_tonal(2, 2000000, -50.0f));
    dbmap_lifecycle_v2_score_t sk =
        score_w(win_keyboard(2, 2000000, -50.0f));
    EXPECT(st.features_valid);
    EXPECT(sk.features_valid);
    EXPECT(st.instant > sk.instant);
    EXPECT(st.instant > 0.55f);
    EXPECT(sk.instant < 0.35f);
}

static void test_stable_tonal_above_minus_55_ends_by_max_age(void)
{
    dbmap_lifecycle_v2_config_t cfg = cfg_fast();
    dbmap_lifecycle_v2_t lc;
    dbmap_lifecycle_v2_init(&lc, &cfg);

    /* Enter above gate (-45), then dwell at -50 (above quiet -55) with high score. */
    dbmap_activity_emission_t e =
        feed_w(&lc, win_tonal(1, 1000000, -40.0f));
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    e = feed_w(&lc, win_tonal(2, 2000000, -40.0f));
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);
    EXPECT(e.interval_start_mono_us == 0);
    EXPECT(e.interval_end_mono_us == 2000000);

    int ends = 0;
    int periodics = 0;
    for (uint32_t seq = 3; seq <= 25; ++seq) {
        e = feed_w(
            &lc, win_tonal(seq, (int64_t)seq * 1000000LL, -50.0f)
        );
        if (e.kind == DBMAP_ACTIVITY_EMIT_PERIODIC) {
            periodics++;
        }
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ends++;
            EXPECT(e.termination == DBMAP_ACTIVITY_TERM_NATURAL);
            EXPECT(lc.ends_max_age >= 1);
            break;
        }
    }
    EXPECT(ends == 1);
    EXPECT(periodics >= 1);
    e = feed_w(&lc, win_tonal(40, 40000000LL, -50.0f));
    EXPECT(e.kind != DBMAP_ACTIVITY_EMIT_END);
}

static void test_keyboard_like_ends_via_signature(void)
{
    dbmap_lifecycle_v2_config_t cfg = cfg_fast();
    cfg.max_episode_ms = 600000;
    dbmap_lifecycle_v2_t lc;
    dbmap_lifecycle_v2_init(&lc, &cfg);

    dbmap_activity_emission_t e =
        feed_w(&lc, win_keyboard(1, 1000000, -40.0f));
    e = feed_w(&lc, win_keyboard(2, 2000000, -40.0f));
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);

    int ends = 0;
    for (uint32_t seq = 3; seq <= 15; ++seq) {
        e = feed_w(
            &lc, win_keyboard(seq, (int64_t)seq * 1000000LL, -48.0f)
        );
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ends++;
            EXPECT(lc.ends_signature >= 1);
            break;
        }
    }
    EXPECT(ends == 1);
}

static void test_quiet_with_occasional_whistle(void)
{
    dbmap_lifecycle_v2_config_t cfg = cfg_fast();
    cfg.max_episode_ms = 600000;
    dbmap_lifecycle_v2_t lc;
    dbmap_lifecycle_v2_init(&lc, &cfg);

    for (uint32_t seq = 1; seq <= 3; ++seq) {
        dbmap_activity_emission_t e = feed_w(
            &lc, win_tonal(seq, (int64_t)seq * 1000000LL, -65.0f)
        );
        EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    }

    dbmap_activity_emission_t e =
        feed_w(&lc, win_tonal(4, 4000000, -35.0f));
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    e = feed_w(&lc, win_tonal(5, 5000000, -35.0f));
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);

    int ends = 0;
    for (uint32_t seq = 6; seq <= 12; ++seq) {
        e = feed_w(
            &lc, win_tonal(seq, (int64_t)seq * 1000000LL, -60.0f)
        );
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ends++;
            EXPECT(lc.ends_quiet >= 1);
            break;
        }
    }
    EXPECT(ends == 1);
}

static void test_signature_gone_rms_still_above_exit(void)
{
    dbmap_lifecycle_v2_config_t cfg = cfg_fast();
    cfg.max_episode_ms = 600000;
    dbmap_lifecycle_v2_t lc;
    dbmap_lifecycle_v2_init(&lc, &cfg);

    dbmap_activity_emission_t e =
        feed_w(&lc, win_tonal(1, 1000000, -40.0f));
    e = feed_w(&lc, win_tonal(2, 2000000, -40.0f));
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);
    (void)feed_w(&lc, win_tonal(3, 3000000, -50.0f));
    (void)feed_w(&lc, win_tonal(4, 4000000, -50.0f));

    int ends = 0;
    for (uint32_t seq = 5; seq <= 20; ++seq) {
        e = feed_w(
            &lc, win_keyboard(seq, (int64_t)seq * 1000000LL, -50.0f)
        );
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ends++;
            EXPECT(lc.ends_signature >= 1);
            EXPECT(lc.ends_quiet == 0);
            break;
        }
    }
    EXPECT(ends == 1);
}

static void test_invalid_features_pause_signature_hold(void)
{
    dbmap_lifecycle_v2_config_t cfg = cfg_fast();
    cfg.max_episode_ms = 600000;
    dbmap_lifecycle_v2_t lc;
    dbmap_lifecycle_v2_init(&lc, &cfg);

    (void)feed_w(&lc, win_keyboard(1, 1000000, -40.0f));
    dbmap_activity_emission_t e =
        feed_w(&lc, win_keyboard(2, 2000000, -40.0f));
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);

    for (uint32_t seq = 3; seq <= 8; ++seq) {
        e = feed_w(
            &lc, win_keyboard(seq, (int64_t)seq * 1000000LL, -48.0f)
        );
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            break;
        }
    }

    if (dbmap_lifecycle_v2_state(&lc) == DBMAP_LC_V2_PENDING_EXIT) {
        dbmap_lifecycle_v2_window_t bad =
            win_base(20, 20000000, -48.0f, 16.0f, 0.5f, 1.5f, 100.0f, false);
        /* Non-consecutive seq pauses hold; must not END this tick. */
        e = dbmap_lifecycle_v2_feed(&lc, &bad);
        EXPECT(e.kind != DBMAP_ACTIVITY_EMIT_END);
    }
}

static void test_pre_trigger_association(void)
{
    dbmap_lifecycle_v2_config_t cfg = cfg_fast();
    cfg.pre_trigger_ms = 3000;
    dbmap_lifecycle_v2_t lc;
    dbmap_lifecycle_v2_init(&lc, &cfg);

    for (uint32_t seq = 1; seq <= 4; ++seq) {
        (void)feed_w(
            &lc, win_tonal(seq, (int64_t)seq * 1000000LL, -70.0f)
        );
    }
    dbmap_activity_emission_t e =
        feed_w(&lc, win_tonal(5, 5000000, -40.0f));
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    e = feed_w(&lc, win_tonal(6, 6000000, -40.0f));
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);
    EXPECT(e.interval_start_mono_us == 1000000LL);
    EXPECT(e.interval_end_mono_us == 6000000);
}

static void test_exactly_one_end(void)
{
    dbmap_lifecycle_v2_config_t cfg = cfg_fast();
    cfg.max_episode_ms = 5000;
    dbmap_lifecycle_v2_t lc;
    dbmap_lifecycle_v2_init(&lc, &cfg);

    (void)feed_w(&lc, win_tonal(1, 1000000, -40.0f));
    dbmap_activity_emission_t e =
        feed_w(&lc, win_tonal(2, 2000000, -40.0f));
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);

    int ends = 0;
    for (uint32_t seq = 3; seq <= 12; ++seq) {
        e = feed_w(
            &lc, win_tonal(seq, (int64_t)seq * 1000000LL, -40.0f)
        );
        if (e.kind == DBMAP_ACTIVITY_EMIT_END) {
            ends++;
            break;
        }
    }
    EXPECT(ends == 1);
    /* Further windows must not emit a second END for the closed episode. */
    e = feed_w(&lc, win_tonal(20, 20000000LL, -70.0f));
    EXPECT(e.kind != DBMAP_ACTIVITY_EMIT_END);
}

static void test_baseline_detector_unaffected_api(void)
{
    dbmap_activity_config_t acfg = {
        .enter_threshold_dbfs = -45.0f,
        .exit_threshold_dbfs = -55.0f,
        .min_activity_ms = 2000,
        .exit_hold_ms = 1000,
        .boot_settle_ms = 0,
        .observation_interval_ms = 5000,
        .max_invalid_hold_ms = 30000,
        .relative_end_enabled = false,
    };
    EXPECT(dbmap_activity_config_valid(&acfg));
    dbmap_activity_detector_t det;
    dbmap_activity_detector_init(&det, &acfg);
    dbmap_activity_sample_t s;
    memset(&s, 0, sizeof(s));
    s.has_measurement = true;
    s.rms_dbfs = -40.0f;
    s.window_ms = 1000;
    s.measured_at_us = 1000000;
    s.window_seq = 1;
    s.ms_since_i2s_enable = 1000;
    s.transport_status = "ok";
    s.microphone_status = "signal_present";
    dbmap_activity_emission_t e = dbmap_activity_detector_feed(&det, &s);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    s.window_seq = 2;
    s.measured_at_us = 2000000;
    e = dbmap_activity_detector_feed(&det, &s);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);
}

int main(void)
{
    test_config_defaults_valid();
    test_score_tonal_vs_keyboard();
    test_stable_tonal_above_minus_55_ends_by_max_age();
    test_keyboard_like_ends_via_signature();
    test_quiet_with_occasional_whistle();
    test_signature_gone_rms_still_above_exit();
    test_invalid_features_pause_signature_hold();
    test_pre_trigger_association();
    test_exactly_one_end();
    test_baseline_detector_unaffected_api();

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
