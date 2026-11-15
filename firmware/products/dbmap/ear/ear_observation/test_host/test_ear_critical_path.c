/**
 * Host tests: Critical Observation Path lifecycle (detection disabled).
 *
 * From repo root:
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     -I firmware/products/dbmap/ear/ear_observation/include \
 *     -I firmware/platform/observation/include \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/ear_observation/ear_critical_path.c \
 *     firmware/platform/observation/episode_registry.c \
 *     firmware/products/dbmap/ear/ear_observation/test_host/test_ear_critical_path.c \
 *     -lm -o /tmp/test_ear_critical_path && /tmp/test_ear_critical_path
 *
 * Source-level / host verification only — not device or Hub MQTT round-trip.
 */
#include "ae_signature.h"
#include "ear_critical_path.h"
#include "sp_episode_registry.h"

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

typedef struct {
    uint32_t starts;
    uint32_t periodics;
    uint32_t ends;
    char last_kind[16];
    char last_term[16];
    char last_origin[32];
    char last_oid[EAR_CRITICAL_OBS_ID_LEN];
    bool fail_next_publish;
} pub_log_t;

static pub_log_t g_pub;
static uint32_t g_uuid_seq;

static void test_uuid(char out[EAR_CRITICAL_OBS_ID_LEN])
{
    g_uuid_seq++;
    snprintf(
        out,
        EAR_CRITICAL_OBS_ID_LEN,
        "00000000-0000-4000-8000-%012u",
        g_uuid_seq
    );
}

static bool test_publish(
    void *user,
    const char *observation_id,
    const char *emission_kind,
    const char *termination_reason,
    const char *origin,
    int64_t interval_start_mono_us,
    int64_t capture_mono_us,
    double interval_duration_ms,
    double episode_duration_ms,
    const ae_signature_t *evidence_or_null
)
{
    (void)user;
    (void)interval_start_mono_us;
    (void)capture_mono_us;
    (void)interval_duration_ms;
    (void)episode_duration_ms;
    (void)evidence_or_null;
    if (g_pub.fail_next_publish) {
        g_pub.fail_next_publish = false;
        return false;
    }
    if (strcmp(emission_kind, "start") == 0) {
        g_pub.starts++;
    } else if (strcmp(emission_kind, "periodic") == 0) {
        g_pub.periodics++;
    } else if (strcmp(emission_kind, "end") == 0) {
        g_pub.ends++;
    }
    snprintf(g_pub.last_kind, sizeof(g_pub.last_kind), "%s", emission_kind);
    if (termination_reason != NULL) {
        snprintf(g_pub.last_term, sizeof(g_pub.last_term), "%s", termination_reason);
    } else {
        g_pub.last_term[0] = '\0';
    }
    snprintf(g_pub.last_origin, sizeof(g_pub.last_origin), "%s", origin ? origin : "");
    snprintf(g_pub.last_oid, sizeof(g_pub.last_oid), "%s", observation_id ? observation_id : "");
    return true;
}

static ae_signature_t make_sig(float rms, float peak, float crest, float b0)
{
    ae_signature_t s;
    memset(&s, 0, sizeof(s));
    s.schema_version = AE_SIGNATURE_SCHEMA_VERSION;
    s.window_seq = 1;
    s.window_end_mono_us = 1000000;
    s.window_ms = 1000;
    s.quality = AE_QUALITY_OK;
    s.rms_dbfs = rms;
    s.peak_dbfs = peak;
    s.crest_db = crest;
    s.band_dbfs[0] = b0;
    s.band_dbfs[1] = b0 - 1.0f;
    s.band_dbfs[2] = b0 - 2.0f;
    s.band_dbfs[3] = b0 - 3.0f;
    return s;
}

static void setup(ear_critical_path_t *cp)
{
    memset(&g_pub, 0, sizeof(g_pub));
    ear_critical_config_t cfg;
    ear_critical_config_set_defaults(&cfg);
    cfg.auto_detect_enabled = false;
    cfg.publish = test_publish;
    cfg.uuid_fn = test_uuid;
    EXPECT(ear_critical_init(cp, &cfg));
}

static void test_force_start_without_registry_slot(void)
{
    printf("test_force_start_without_registry_slot\n");
    ear_critical_path_t cp;
    setup(&cp);
    sp_episode_registry_t reg;
    sp_episode_registry_init(&reg);
    /* Fill both normal slots — Critical must not need one. */
    sp_ep_handle_t h0, h1, h_full;
    EXPECT(sp_episode_registry_allocate(&reg, &h0, 0, 5000, 1, test_uuid, false));
    EXPECT(sp_episode_registry_allocate(&reg, &h1, 0, 5000, 1, test_uuid, false));
    EXPECT(sp_episode_registry_allocate(&reg, &h_full, 0, 5000, 1, test_uuid, false) == false);
    const char *id0 = sp_episode_registry_get(&reg, h0)->observation_id;
    const char *id1 = sp_episode_registry_get(&reg, h1)->observation_id;

    ae_signature_t sig = make_sig(-40.0f, -2.0f, 38.0f, -45.0f);
    EXPECT(ear_critical_force_start(&cp, &sig, 1000000));
    EXPECT(ear_critical_is_active(&cp));
    EXPECT(g_pub.starts == 1);
    EXPECT(strcmp(g_pub.last_origin, EAR_CRITICAL_ORIGIN) == 0);
    char oid[EAR_CRITICAL_OBS_ID_LEN];
    EXPECT(ear_critical_active_observation_id(&cp, oid));
    EXPECT(strcmp(oid, id0) != 0);
    EXPECT(strcmp(oid, id1) != 0);
}

static void test_start_periodic_end(void)
{
    printf("test_start_periodic_end\n");
    ear_critical_path_t cp;
    setup(&cp);
    ae_signature_t sig = make_sig(-40.0f, -2.0f, 38.0f, -45.0f);
    EXPECT(ear_critical_force_start(&cp, &sig, 0));
    /* Just under idle/max; advance past PERIODIC 5s */
    ear_critical_poll(&cp, 5000000);
    EXPECT(g_pub.periodics == 1);
    EXPECT(strcmp(g_pub.last_origin, "critical_path") == 0);
    ear_critical_force_end(&cp, 6000000, EAR_CRITICAL_END_EXPLICIT);
    EXPECT(!ear_critical_is_active(&cp));
    EXPECT(g_pub.ends == 1);
    EXPECT(strcmp(g_pub.last_term, "natural") == 0);
}

static void test_idle_and_max_timeout(void)
{
    printf("test_idle_and_max_timeout\n");
    ear_critical_path_t cp;
    setup(&cp);
    ae_signature_t sig = make_sig(-40.0f, -2.0f, 38.0f, -45.0f);
    EXPECT(ear_critical_force_start(&cp, &sig, 0));
    /* Idle 15s without match */
    ear_critical_poll(&cp, 15000000);
    EXPECT(!ear_critical_is_active(&cp));
    ear_critical_diag_t d;
    ear_critical_get_diag(&cp, &d);
    EXPECT(d.end_idle == 1);

    setup(&cp);
    EXPECT(ear_critical_force_start(&cp, &sig, 0));
    /* Keep matching so idle does not fire; stay under max until final poll */
    for (int i = 1; i <= 5; ++i) {
        ae_signature_t m = make_sig(-40.0f, -2.0f, 38.0f, -45.0f);
        m.window_seq = (uint32_t)i + 1u;
        ear_critical_inject_evidence(&cp, &m, (int64_t)i * 5000000LL);
        ear_critical_poll(&cp, (int64_t)i * 5000000LL);
    }
    EXPECT(ear_critical_is_active(&cp));
    ae_signature_t keep = make_sig(-40.0f, -2.0f, 38.0f, -45.0f);
    keep.window_seq = 99;
    ear_critical_inject_evidence(&cp, &keep, 29900000);
    ear_critical_poll(&cp, 30000000);
    EXPECT(!ear_critical_is_active(&cp));
    ear_critical_get_diag(&cp, &d);
    EXPECT(d.end_max == 1);
}

static void test_match_resets_idle_background_does_not(void)
{
    printf("test_match_resets_idle_background_does_not\n");
    ear_critical_path_t cp;
    setup(&cp);
    ae_signature_t start = make_sig(-40.0f, -2.0f, 38.0f, -45.0f);
    EXPECT(ear_critical_force_start(&cp, &start, 0));

    /* Background: low crest, far bands — must not reset idle */
    ae_signature_t bg = make_sig(-55.0f, -50.0f, 5.0f, -70.0f);
    bg.window_seq = 2;
    ear_critical_inject_evidence(&cp, &bg, 1000000);
    ear_critical_diag_t d;
    ear_critical_get_diag(&cp, &d);
    EXPECT(d.bg_ignored >= 1);
    EXPECT(d.match_reset == 0);

    /* At 14s still active (idle 15s); background did not help */
    ear_critical_poll(&cp, 14000000);
    EXPECT(ear_critical_is_active(&cp));

    /* Matching follow-up at 14s resets idle */
    ae_signature_t match = make_sig(-40.1f, -2.1f, 38.0f, -45.2f);
    match.window_seq = 3;
    ear_critical_inject_evidence(&cp, &match, 14000000);
    ear_critical_get_diag(&cp, &d);
    EXPECT(d.match_reset >= 1);

    /* 14s + 14s = 28s from start; idle from last match at 14s → still active at 28s */
    ear_critical_poll(&cp, 28000000);
    EXPECT(ear_critical_is_active(&cp));

    /* 14s + 15s = 29s → idle END (before max 30s) */
    ear_critical_poll(&cp, 29000000);
    EXPECT(!ear_critical_is_active(&cp));
}

static void test_second_trigger_dropped(void)
{
    printf("test_second_trigger_dropped\n");
    ear_critical_path_t cp;
    setup(&cp);
    ae_signature_t a = make_sig(-40.0f, -2.0f, 38.0f, -45.0f);
    ae_signature_t b = make_sig(-38.0f, -1.0f, 37.0f, -44.0f);
    b.window_seq = 9;
    EXPECT(ear_critical_force_start(&cp, &a, 0));
    char oid1[EAR_CRITICAL_OBS_ID_LEN];
    EXPECT(ear_critical_active_observation_id(&cp, oid1));
    EXPECT(!ear_critical_force_start(&cp, &b, 1000000));
    ear_critical_diag_t d;
    ear_critical_get_diag(&cp, &d);
    EXPECT(d.drop_busy == 1);
    EXPECT(g_pub.starts == 1);
    char oid2[EAR_CRITICAL_OBS_ID_LEN];
    EXPECT(ear_critical_active_observation_id(&cp, oid2));
    EXPECT(strcmp(oid1, oid2) == 0);
}

static void test_restart_after_end(void)
{
    printf("test_restart_after_end\n");
    ear_critical_path_t cp;
    setup(&cp);
    ae_signature_t sig = make_sig(-40.0f, -2.0f, 38.0f, -45.0f);
    EXPECT(ear_critical_force_start(&cp, &sig, 0));
    char oid1[EAR_CRITICAL_OBS_ID_LEN];
    EXPECT(ear_critical_active_observation_id(&cp, oid1));
    ear_critical_force_end(&cp, 1000000, EAR_CRITICAL_END_EXPLICIT);
    EXPECT(ear_critical_force_start(&cp, &sig, 2000000));
    char oid2[EAR_CRITICAL_OBS_ID_LEN];
    EXPECT(ear_critical_active_observation_id(&cp, oid2));
    EXPECT(strcmp(oid1, oid2) != 0);
    EXPECT(g_pub.starts == 2);
}

static void test_ota_cleanup(void)
{
    printf("test_ota_cleanup\n");
    ear_critical_path_t cp;
    setup(&cp);
    ae_signature_t sig = make_sig(-40.0f, -2.0f, 38.0f, -45.0f);
    EXPECT(ear_critical_force_start(&cp, &sig, 0));
    ear_critical_close_for_ota(&cp, 5000000);
    EXPECT(!ear_critical_is_active(&cp));
    ear_critical_diag_t d;
    ear_critical_get_diag(&cp, &d);
    EXPECT(d.end_ota == 1);
    EXPECT(strcmp(g_pub.last_term, "ota_reboot") == 0);
}

static void test_end_releases_even_if_publish_fails(void)
{
    printf("test_end_releases_even_if_publish_fails\n");
    ear_critical_path_t cp;
    setup(&cp);
    ae_signature_t sig = make_sig(-40.0f, -2.0f, 38.0f, -45.0f);
    EXPECT(ear_critical_force_start(&cp, &sig, 0));
    g_pub.fail_next_publish = true;
    ear_critical_force_end(&cp, 1000000, EAR_CRITICAL_END_EXPLICIT);
    EXPECT(!ear_critical_is_active(&cp));
    ear_critical_diag_t d;
    ear_critical_get_diag(&cp, &d);
    EXPECT(d.publish_fail >= 1);
    EXPECT(d.end_explicit == 1);
}

static void test_auto_detect_never_starts(void)
{
    printf("test_auto_detect_never_starts\n");
    ear_critical_path_t cp;
    setup(&cp);
    /* Production auto_detect flag alone never STARTS (no approved detector). */
    cp.cfg.auto_detect_enabled = true;
    cp.cfg.lab_impulse_enabled = false;
    ae_signature_t loud = make_sig(-40.0f, -0.5f, 40.0f, -45.0f);
    ear_critical_on_signature(&cp, &loud, 1000000);
    EXPECT(!ear_critical_is_active(&cp));
    EXPECT(g_pub.starts == 0);
    ear_critical_diag_t d;
    ear_critical_get_diag(&cp, &d);
    EXPECT(d.auto_start_blocked >= 1);
}

static void test_lab_impulse_starts_and_times(void)
{
    printf("test_lab_impulse_starts_and_times\n");
    ear_critical_path_t cp;
    setup(&cp);
    cp.cfg.lab_impulse_enabled = true;
    cp.cfg.lab_crest_floor_db = 12.0f;
    cp.cfg.lab_peak_dbfs_min = -20.0f;

    ae_signature_t quiet = make_sig(-55.0f, -50.0f, 5.0f, -60.0f);
    quiet.window_end_mono_us = 900000;
    ear_critical_on_signature(&cp, &quiet, 1000000);
    EXPECT(!ear_critical_is_active(&cp));
    ear_critical_diag_t d;
    ear_critical_get_diag(&cp, &d);
    EXPECT(d.lab_eval >= 1);
    EXPECT(d.lab_trigger == 0);

    ae_signature_t impulse = make_sig(-40.0f, -2.0f, 38.0f, -45.0f);
    impulse.window_end_mono_us = 1000000;
    ear_critical_on_signature(&cp, &impulse, 1050000);
    EXPECT(ear_critical_is_active(&cp));
    EXPECT(g_pub.starts == 1);
    ear_critical_get_diag(&cp, &d);
    EXPECT(d.lab_trigger == 1);
    EXPECT(d.lab_start_ok == 1);
    EXPECT(d.lab_detect_us_last == 50000u);
}

static void test_on_signature_idle_reset_when_active(void)
{
    printf("test_on_signature_idle_reset_when_active\n");
    ear_critical_path_t cp;
    setup(&cp);
    ae_signature_t start = make_sig(-40.0f, -2.0f, 38.0f, -45.0f);
    EXPECT(ear_critical_force_start(&cp, &start, 0));
    ae_signature_t match = make_sig(-40.0f, -2.0f, 38.0f, -45.0f);
    match.window_seq = 2;
    ear_critical_on_signature(&cp, &match, 1000000);
    ear_critical_diag_t d;
    ear_critical_get_diag(&cp, &d);
    EXPECT(d.match_reset >= 1);
}

int main(void)
{
    g_uuid_seq = 0;
    test_force_start_without_registry_slot();
    test_start_periodic_end();
    test_idle_and_max_timeout();
    test_match_resets_idle_background_does_not();
    test_second_trigger_dropped();
    test_restart_after_end();
    test_ota_cleanup();
    test_end_releases_even_if_publish_fails();
    test_auto_detect_never_starts();
    test_lab_impulse_starts_and_times();
    test_on_signature_idle_reset_when_active();

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
