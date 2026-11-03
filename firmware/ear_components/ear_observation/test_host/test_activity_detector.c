/**
 * Host unit tests for RMS activity state machine + emission policy.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     firmware/ear_components/ear_observation/activity_detector.c \
 *     firmware/ear_components/ear_observation/test_host/test_activity_detector.c \
 *     -lm -o /tmp/test_dbmap_activity_detector && /tmp/test_dbmap_activity_detector
 */
#include "dbmap_activity_detector.h"

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

static dbmap_activity_config_t test_cfg(void)
{
    dbmap_activity_config_t cfg = {
        .enter_threshold_dbfs = -45.0f,
        .exit_threshold_dbfs = -55.0f,
        .min_activity_ms = 2000,
        .exit_hold_ms = 1000,
        .boot_settle_ms = 3000,
        .observation_interval_ms = 5000,
        .max_invalid_hold_ms = 30000,
    };
    return cfg;
}

static dbmap_activity_sample_t sample(
    uint32_t seq,
    int64_t measured_at_us,
    uint32_t ms_since_i2s,
    float rms,
    const char *transport,
    const char *mic
)
{
    dbmap_activity_sample_t s;
    memset(&s, 0, sizeof(s));
    s.has_measurement = true;
    s.rms_dbfs = rms;
    s.window_ms = 1000;
    s.measured_at_us = measured_at_us;
    s.window_seq = seq;
    s.ms_since_i2s_enable = ms_since_i2s;
    s.transport_status = transport;
    s.microphone_status = mic;
    return s;
}

static dbmap_activity_emission_t feed(
    dbmap_activity_detector_t *det,
    uint32_t seq,
    int64_t measured_at_us,
    uint32_t ms_since_i2s,
    float rms,
    const char *transport,
    const char *mic
)
{
    dbmap_activity_sample_t s = sample(seq, measured_at_us, ms_since_i2s, rms, transport, mic);
    return dbmap_activity_detector_feed(det, &s);
}

static void test_config_rejects_bad_hysteresis(void)
{
    dbmap_activity_config_t cfg = test_cfg();
    EXPECT(dbmap_activity_config_valid(&cfg));
    cfg.exit_threshold_dbfs = -40.0f;
    EXPECT(!dbmap_activity_config_valid(&cfg));
}

static void test_usable_allows_unknown_rejects_faults(void)
{
    dbmap_activity_sample_t ok_unknown =
        sample(1, 1000, 1000, -60.0f, "ok", "unknown");
    EXPECT(dbmap_activity_sample_usable(&ok_unknown));

    dbmap_activity_sample_t ok_present =
        sample(1, 1000, 1000, -60.0f, "ok", "signal_present");
    EXPECT(dbmap_activity_sample_usable(&ok_present));

    dbmap_activity_sample_t noise =
        sample(1, 1000, 1000, -20.0f, "ok", "noise_suspected");
    EXPECT(!dbmap_activity_sample_usable(&noise));

    dbmap_activity_sample_t disc =
        sample(1, 1000, 1000, -120.0f, "ok", "disconnected_suspected");
    EXPECT(!dbmap_activity_sample_usable(&disc));

    dbmap_activity_sample_t bad_tr =
        sample(1, 1000, 1000, -40.0f, "error", "signal_present");
    EXPECT(!dbmap_activity_sample_usable(&bad_tr));
}

static void test_weak_unknown_can_enter_via_rms(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = test_cfg();
    cfg.boot_settle_ms = 0;
    cfg.min_activity_ms = 1000;
    cfg.enter_threshold_dbfs = -70.0f;
    cfg.exit_threshold_dbfs = -80.0f;
    dbmap_activity_detector_init(&det, &cfg);

    /* Weak RMS, diagnostic unknown — must not be gated out before thresholds. */
    dbmap_activity_emission_t e =
        feed(&det, 1, 1000000, 1000, -65.0f, "ok", "unknown");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
}

static void test_boot_settle_and_invalid_hold(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = test_cfg();
    dbmap_activity_detector_init(&det, &cfg);

    dbmap_activity_emission_t e =
        feed(&det, 1, 1000000, 1000, -30.0f, "ok", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    EXPECT(det.windows_boot_settle == 1);

    e = feed(&det, 2, 4000000, 4000, -20.0f, "ok", "noise_suspected");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
    EXPECT(det.windows_invalid >= 1);

    e = feed(&det, 3, 5000000, 5000, -20.0f, "error", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
}

static void test_start_periodic_end(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = test_cfg();
    cfg.boot_settle_ms = 0;
    dbmap_activity_detector_init(&det, &cfg);

    dbmap_activity_emission_t e =
        feed(&det, 1, 1000000, 1000, -40.0f, "ok", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_ENTER);

    e = feed(&det, 2, 2000000, 2000, -40.0f, "ok", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(e.duration_ms == 2000.0);

    e = feed(&det, 3, 3000000, 3000, -50.0f, "ok", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    e = feed(&det, 4, 4000000, 4000, -50.0f, "ok", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    e = feed(&det, 5, 5000000, 5000, -40.0f, "ok", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    e = feed(&det, 6, 6000000, 6000, -40.0f, "ok", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    e = feed(&det, 7, 7000000, 7000, -40.0f, "ok", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_PERIODIC);
    EXPECT(e.duration_ms == 5000.0);

    e = feed(&det, 8, 8000000, 8000, -60.0f, "ok", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);

    e = feed(&det, 9, 9000000, 9000, -60.0f, "ok", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
    EXPECT(e.duration_ms == 2000.0);
}

static void test_invalid_does_not_end_as_silence(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = test_cfg();
    cfg.boot_settle_ms = 0;
    cfg.min_activity_ms = 1000;
    cfg.max_invalid_hold_ms = 30000;
    dbmap_activity_detector_init(&det, &cfg);

    dbmap_activity_emission_t e =
        feed(&det, 1, 1000000, 1000, -40.0f, "ok", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);

    /* Short invalid burst: hold ACTIVE, do not END. */
    e = feed(&det, 2, 2000000, 2000, -120.0f, "ok", "disconnected_suspected");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);

    e = feed(&det, 3, 3000000, 3000, -40.0f, "ok", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
}

static void test_forced_end_after_max_invalid_hold(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = test_cfg();
    cfg.boot_settle_ms = 0;
    cfg.min_activity_ms = 1000;
    cfg.max_invalid_hold_ms = 3000;
    dbmap_activity_detector_init(&det, &cfg);

    dbmap_activity_emission_t e =
        feed(&det, 1, 1000000, 1000, -40.0f, "ok", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);

    e = feed(&det, 2, 2000000, 2000, -120.0f, "ok", "disconnected_suspected");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);

    e = feed(&det, 3, 3000000, 3000, -120.0f, "ok", "disconnected_suspected");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);

    /* 1s valid + 3s invalid ≥ max_invalid_hold → forced END at last valid (1s). */
    e = feed(&det, 4, 4000000, 4000, -120.0f, "ok", "disconnected_suspected");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(e.termination == DBMAP_ACTIVITY_TERM_INVALID_HOLD);
    EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
    EXPECT(det.forced_ends_invalid == 1);
    EXPECT(e.interval_end_mono_us == 1000000);
    EXPECT(e.duration_ms == 0.0); /* last_emit == last_valid == 1s */
}

static void test_duplicate_window_ignored(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = test_cfg();
    cfg.boot_settle_ms = 0;
    cfg.min_activity_ms = 1000;
    dbmap_activity_detector_init(&det, &cfg);

    dbmap_activity_sample_t s = sample(1, 1000000, 1000, -40.0f, "ok", "signal_present");
    dbmap_activity_emission_t e = dbmap_activity_detector_feed(&det, &s);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);
    e = dbmap_activity_detector_feed(&det, &s);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    EXPECT(det.windows_seen == 1);
}

static void test_mqtt_independence_contract(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = test_cfg();
    cfg.boot_settle_ms = 0;
    cfg.min_activity_ms = 1000;
    dbmap_activity_detector_init(&det, &cfg);

    dbmap_activity_emission_t e =
        feed(&det, 1, 1000000, 1000, -40.0f, "ok", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);
    e = feed(&det, 2, 2000000, 2000, -40.0f, "ok", "signal_present");
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);
    e = feed(&det, 3, 3000000, 3000, -60.0f, "ok", "signal_present");
    EXPECT(det.state == DBMAP_ACTIVITY_PENDING_EXIT);
    e = feed(&det, 4, 4000000, 4000, -60.0f, "ok", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(e.termination == DBMAP_ACTIVITY_TERM_NATURAL);
    EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
}

/** NEW: force_end / reset_idle for OTA close paths. */
static void test_force_end_and_reset_idle(void)
{
    dbmap_activity_detector_t det;
    dbmap_activity_config_t cfg = test_cfg();
    cfg.boot_settle_ms = 0;
    cfg.min_activity_ms = 1000;
    dbmap_activity_detector_init(&det, &cfg);

    dbmap_activity_emission_t e =
        feed(&det, 1, 1000000, 1000, -40.0f, "ok", "signal_present");
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_START);
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);

    e = dbmap_activity_detector_force_end(&det);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_END);
    EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
    EXPECT(e.interval_end_mono_us == 1000000);

    e = dbmap_activity_detector_force_end(&det);
    EXPECT(e.kind == DBMAP_ACTIVITY_EMIT_NONE);

    feed(&det, 2, 2000000, 2000, -40.0f, "ok", "signal_present");
    EXPECT(det.state == DBMAP_ACTIVITY_ACTIVE);
    dbmap_activity_detector_reset_idle(&det);
    EXPECT(det.state == DBMAP_ACTIVITY_IDLE);
}

int main(void)
{
    /* Existing */
    test_config_rejects_bad_hysteresis();
    test_usable_allows_unknown_rejects_faults();
    test_weak_unknown_can_enter_via_rms();
    test_boot_settle_and_invalid_hold();
    test_start_periodic_end();
    test_invalid_does_not_end_as_silence();
    test_forced_end_after_max_invalid_hold();
    test_duplicate_window_ignored();
    test_mqtt_independence_contract();
    /* Newly added */
    test_force_end_and_reset_idle();

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
