/**
 * Integration host tests: Critical Path vs normal Obs slots + OTA cleanup.
 *
 * Test 4 — Critical with both normal Observation slots occupied.
 * Test 5 — OTA product-binding cleanup while Critical is active (+ boot boundary).
 *
 * Uses production ear_obs_manager, episode registry, ear_critical_path, and
 * ear_obs_ota_release_product_bindings (same helper as ear_observation_close_for_ota).
 *
 * Automatic acoustic detection remains disabled (force_start only).
 *
 * From repo root:
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/candidate_engine/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     -I firmware/products/dbmap/ear/ear_observation/include \
 *     -I firmware/platform/observation/include \
 *     firmware/products/dbmap/ear/ear_acoustic/acoustic_l0.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_history.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_time.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_engine.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_decision.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_module.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_workflow.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_manager.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_engine.c \
 *     firmware/platform/observation/episode_registry.c \
 *     firmware/products/dbmap/ear/ear_observation/ear_obs_manager.c \
 *     firmware/products/dbmap/ear/ear_observation/ear_promotion_policy.c \
 *     firmware/products/dbmap/ear/ear_observation/ear_critical_path.c \
 *     firmware/products/dbmap/ear/ear_observation/ear_obs_ota_cleanup.c \
 *     firmware/products/dbmap/ear/ear_observation/test_host/test_ear_critical_integration.c \
 *     -lm -o /tmp/test_ear_critical_integration && /tmp/test_ear_critical_integration
 *
 * Host / source-level only — not physical reboot or Hub MQTT verification.
 */
#include "ae_engine.h"
#include "ae_signature.h"
#include "as_profile.h"
#include "ce_engine.h"
#include "ear_critical_path.h"
#include "ear_obs_manager.h"
#include "ear_obs_ota_cleanup.h"
#include "ear_promotion_policy.h"
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

/* ---- normal Obs emit log ---- */

typedef struct {
    uint32_t starts;
    uint32_t periodics;
    uint32_t ends;
    char ids[SP_OBS_EPISODE_SLOTS][SP_OBS_EPISODE_ID_LEN];
    uint32_t n_ids;
} normal_emit_t;

static normal_emit_t g_norm;

static bool norm_emit_start(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    const ear_obs_promote_snapshot_t *snap,
    int64_t now_mono_us
)
{
    (void)ctx;
    (void)snap;
    (void)now_mono_us;
    g_norm.starts++;
    if (g_norm.n_ids < SP_OBS_EPISODE_SLOTS) {
        memcpy(
            g_norm.ids[g_norm.n_ids],
            binding->observation_id,
            SP_OBS_EPISODE_ID_LEN
        );
        g_norm.n_ids++;
    }
    return true;
}

static bool norm_emit_periodic(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    int64_t a,
    int64_t b,
    bool has_lvl,
    float lvl
)
{
    (void)ctx;
    (void)binding;
    (void)a;
    (void)b;
    (void)has_lvl;
    (void)lvl;
    g_norm.periodics++;
    return true;
}

static bool norm_emit_end(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    ear_obs_end_reason_t reason,
    int64_t a,
    int64_t b,
    bool has_lvl,
    float lvl
)
{
    (void)ctx;
    (void)binding;
    (void)reason;
    (void)a;
    (void)b;
    (void)has_lvl;
    (void)lvl;
    g_norm.ends++;
    return true;
}

/* ---- Critical publish log ---- */

typedef struct {
    uint32_t starts;
    uint32_t periodics;
    uint32_t ends;
    char last_oid[EAR_CRITICAL_OBS_ID_LEN];
    char last_kind[16];
    char last_term[16];
    char last_origin[32];
} crit_pub_t;

static crit_pub_t g_crit;

static bool crit_publish(
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
    if (strcmp(emission_kind, "start") == 0) {
        g_crit.starts++;
    } else if (strcmp(emission_kind, "periodic") == 0) {
        g_crit.periodics++;
    } else if (strcmp(emission_kind, "end") == 0) {
        g_crit.ends++;
    }
    snprintf(g_crit.last_oid, sizeof(g_crit.last_oid), "%s", observation_id ? observation_id : "");
    snprintf(g_crit.last_kind, sizeof(g_crit.last_kind), "%s", emission_kind ? emission_kind : "");
    snprintf(
        g_crit.last_term,
        sizeof(g_crit.last_term),
        "%s",
        termination_reason ? termination_reason : ""
    );
    snprintf(g_crit.last_origin, sizeof(g_crit.last_origin), "%s", origin ? origin : "");
    return true;
}

static uint32_t g_uuid_n;

static void fake_uuid(char out[SP_OBS_EPISODE_ID_LEN])
{
    snprintf(
        out,
        SP_OBS_EPISODE_ID_LEN,
        "aaaaaaaa-bbbb-4ccc-8ddd-%012u",
        (unsigned)g_uuid_n++
    );
}

static void crit_uuid(char out[EAR_CRITICAL_OBS_ID_LEN])
{
    snprintf(
        out,
        EAR_CRITICAL_OBS_ID_LEN,
        "cccccccc-bbbb-4ccc-8ddd-%012u",
        (unsigned)g_uuid_n++
    );
}

static ae_candidate_t *plant(
    ce_engine_t *ce,
    uint64_t id,
    ae_cand_state_t state,
    uint32_t evidence,
    float ema
)
{
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (ce->slots[i].in_use) {
            continue;
        }
        memset(&ce->slots[i], 0, sizeof(ce->slots[i]));
        ce->slots[i].in_use = true;
        ce->slots[i].candidate_id = id;
        ce->slots[i].state = state;
        ce->slots[i].evidence_count = evidence;
        ce->slots[i].ema_rms_dbfs = ema;
        ce->slots[i].quality_summary = AE_QUALITY_OK;
        ce->slots[i].last_evidence_mono_us = (int64_t)id * 1000;
        if (id >= ce->next_id) {
            ce->next_id = id + 1u;
        }
        return &ce->slots[i];
    }
    EXPECT(0 && "no free CE slot");
    return NULL;
}

static ae_signature_t make_impulse_sig(uint32_t seq)
{
    ae_signature_t s;
    memset(&s, 0, sizeof(s));
    s.schema_version = AE_SIGNATURE_SCHEMA_VERSION;
    s.window_seq = seq;
    s.window_end_mono_us = (int64_t)seq * 1000000;
    s.window_ms = 1000;
    s.quality = AE_QUALITY_OK;
    s.rms_dbfs = -40.0f;
    s.peak_dbfs = -2.0f;
    s.crest_db = 38.0f;
    s.band_dbfs[0] = -45.0f;
    s.band_dbfs[1] = -46.0f;
    s.band_dbfs[2] = -47.0f;
    s.band_dbfs[3] = -48.0f;
    return s;
}

typedef struct {
    ce_engine_t ce;
    ae_engine_t ae;
    sp_episode_registry_t reg;
    ear_obs_manager_t om;
    ear_critical_path_t crit;
} integ_t;

static void snapshot_registry(
    const sp_episode_registry_t *reg,
    bool in_use[SP_OBS_EPISODE_SLOTS],
    char ids[SP_OBS_EPISODE_SLOTS][SP_OBS_EPISODE_ID_LEN],
    uint16_t gens[SP_OBS_EPISODE_SLOTS]
)
{
    for (uint8_t i = 0; i < SP_OBS_EPISODE_SLOTS; ++i) {
        in_use[i] = reg->slots[i].in_use;
        gens[i] = reg->slots[i].generation;
        if (reg->slots[i].in_use) {
            memcpy(ids[i], reg->slots[i].observation_id, SP_OBS_EPISODE_ID_LEN);
        } else {
            ids[i][0] = '\0';
        }
    }
}

static void mark_as001_relevant(ce_engine_t *ce, uint64_t id)
{
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (!ce->slots[i].in_use || ce->slots[i].candidate_id != id) {
            continue;
        }
        ce_classification_t *cls = &ce->classifications[i];
        memset(cls, 0, sizeof(*cls));
        cls->candidate_id = id;
        cls->valid = true;
        cls->matched = true;
        cls->profile_id = AS_PROFILE_ID_AS001;
        cls->acoustic_class = AS_CLASS_HELICOPTER_LIKE;
        cls->distance = 1.0f;
        cls->gap_events_at_match = ce->slots[i].gap_events;
        return;
    }
}

static bool promote_with_reserve(integ_t *f, uint64_t cid, int64_t now_us)
{
    mark_as001_relevant(&f->ce, cid);
    ear_obs_rsv_handle_t h;
    if (ear_obs_manager_reserve(&f->om, &h) != EAR_OBS_RSV_OK) {
        return false;
    }
    if (!ear_obs_manager_bind_reservation_candidate(&f->om, h, cid)) {
        ear_obs_manager_release_reservation(&f->om, h);
        return false;
    }
    if (!ear_obs_manager_stamp_reservation_relevance(
            &f->om, h, cid, AS_PROFILE_ID_AS001, 1.0f
        )) {
        ear_obs_manager_release_reservation(&f->om, h);
        return false;
    }
    if (!ce_promote(&f->ce, cid)) {
        ear_obs_manager_release_reservation(&f->om, h);
        return false;
    }
    ear_obs_manager_poll(&f->om, now_us);
    return true;
}

static void integ_init(integ_t *f)
{
    memset(f, 0, sizeof(*f));
    memset(&g_norm, 0, sizeof(g_norm));
    memset(&g_crit, 0, sizeof(g_crit));
    g_uuid_n = 1;

    ce_config_t ccfg;
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 1u;
    EXPECT(ce_start(&f->ce, &ccfg));

    ae_config_t acfg;
    ae_config_set_defaults(&acfg);
    EXPECT(ae_start(&f->ae, &acfg));
    EXPECT(ce_bind_audio_engine(&f->ce, &f->ae));

    sp_episode_registry_init(&f->reg);

    ear_obs_manager_config_t mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.registry = &f->reg;
    mcfg.ce = &f->ce;
    mcfg.ae = &f->ae;
    mcfg.uuid_fn = fake_uuid;
    mcfg.observation_interval_ms = 1000u;
    mcfg.pending_timeout_ms = 30000u;
    mcfg.producer_tag = 2u;
    mcfg.emit_start = norm_emit_start;
    mcfg.emit_periodic = norm_emit_periodic;
    mcfg.emit_end = norm_emit_end;
    mcfg.enable_poll_periodic = true;
    ear_obs_manager_init(&f->om, &mcfg);
    ce_set_promotion_cb(&f->ce, ear_obs_manager_on_promotion, &f->om);

    ear_critical_config_t ccfg_c;
    ear_critical_config_set_defaults(&ccfg_c);
    ccfg_c.auto_detect_enabled = false;
    ccfg_c.publish = crit_publish;
    ccfg_c.uuid_fn = crit_uuid;
    ccfg_c.periodic_ms = 5000u;
    ccfg_c.idle_timeout_ms = 15000u;
    ccfg_c.max_active_ms = 30000u;
    EXPECT(ear_critical_init(&f->crit, &ccfg_c));
}

static void integ_fini(integ_t *f)
{
    ce_stop(&f->ce);
}

/* ========== Test 4 ========== */

static void test4_critical_with_both_normal_slots_occupied(void)
{
    printf("test4_critical_with_both_normal_slots_occupied\n");
    integ_t f;
    integ_init(&f);

    plant(&f.ce, 11u, AE_CAND_ACTIVE, 5u, -30.0f);
    plant(&f.ce, 12u, AE_CAND_ACTIVE, 5u, -31.0f);
    EXPECT(promote_with_reserve(&f, 11u, 1000000));
    EXPECT(promote_with_reserve(&f, 12u, 2000000));

    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 2u);
    EXPECT(g_norm.starts == 2u);
    EXPECT(g_norm.n_ids == 2u);

    bool before_use[SP_OBS_EPISODE_SLOTS];
    char before_ids[SP_OBS_EPISODE_SLOTS][SP_OBS_EPISODE_ID_LEN];
    uint16_t before_gen[SP_OBS_EPISODE_SLOTS];
    snapshot_registry(&f.reg, before_use, before_ids, before_gen);
    EXPECT(before_use[0] && before_use[1]);

    const uint32_t norm_starts_before = g_norm.starts;
    const uint32_t norm_ends_before = g_norm.ends;
    const uint32_t norm_periodics_before = g_norm.periodics;

    ae_signature_t sig = make_impulse_sig(10);
    EXPECT(ear_critical_force_start(&f.crit, &sig, 3000000));
    EXPECT(ear_critical_is_active(&f.crit));
    EXPECT(g_crit.starts == 1u);
    EXPECT(strcmp(g_crit.last_origin, EAR_CRITICAL_ORIGIN) == 0);
    EXPECT(strcmp(g_crit.last_kind, "start") == 0);

    char crit_oid[EAR_CRITICAL_OBS_ID_LEN];
    EXPECT(ear_critical_active_observation_id(&f.crit, crit_oid));
    EXPECT(strcmp(crit_oid, before_ids[0]) != 0);
    EXPECT(strcmp(crit_oid, before_ids[1]) != 0);
    EXPECT(strcmp(crit_oid, g_norm.ids[0]) != 0);
    EXPECT(strcmp(crit_oid, g_norm.ids[1]) != 0);

    /* Normal Obs unchanged */
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 2u);
    EXPECT(g_norm.starts == norm_starts_before);
    EXPECT(g_norm.ends == norm_ends_before);

    bool after_use[SP_OBS_EPISODE_SLOTS];
    char after_ids[SP_OBS_EPISODE_SLOTS][SP_OBS_EPISODE_ID_LEN];
    uint16_t after_gen[SP_OBS_EPISODE_SLOTS];
    snapshot_registry(&f.reg, after_use, after_ids, after_gen);
    for (uint8_t i = 0; i < SP_OBS_EPISODE_SLOTS; ++i) {
        EXPECT(after_use[i] == before_use[i]);
        EXPECT(after_gen[i] == before_gen[i]);
        EXPECT(strcmp(after_ids[i], before_ids[i]) == 0);
    }

    /* Critical PERIODIC + END */
    ear_critical_poll(&f.crit, 8000000); /* +5s */
    EXPECT(g_crit.periodics == 1u);
    EXPECT(strcmp(g_crit.last_origin, EAR_CRITICAL_ORIGIN) == 0);

    ear_critical_force_end(&f.crit, 9000000, EAR_CRITICAL_END_EXPLICIT);
    EXPECT(!ear_critical_is_active(&f.crit));
    EXPECT(g_crit.ends == 1u);
    EXPECT(strcmp(g_crit.last_term, "natural") == 0);

    /* Normal still untouched after Critical END */
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 2u);
    EXPECT(g_norm.ends == norm_ends_before);
    EXPECT(g_norm.periodics == norm_periodics_before);
    snapshot_registry(&f.reg, after_use, after_ids, after_gen);
    for (uint8_t i = 0; i < SP_OBS_EPISODE_SLOTS; ++i) {
        EXPECT(strcmp(after_ids[i], before_ids[i]) == 0);
        EXPECT(after_gen[i] == before_gen[i]);
    }

    /* Second Critical START while active is dropped (re-start then drop) */
    ae_signature_t sig2 = make_impulse_sig(20);
    EXPECT(ear_critical_force_start(&f.crit, &sig2, 10000000));
    char oid_a[EAR_CRITICAL_OBS_ID_LEN];
    EXPECT(ear_critical_active_observation_id(&f.crit, oid_a));
    ae_signature_t sig3 = make_impulse_sig(21);
    EXPECT(!ear_critical_force_start(&f.crit, &sig3, 11000000));
    ear_critical_diag_t d;
    ear_critical_get_diag(&f.crit, &d);
    EXPECT(d.drop_busy >= 1u);
    char oid_b[EAR_CRITICAL_OBS_ID_LEN];
    EXPECT(ear_critical_active_observation_id(&f.crit, oid_b));
    EXPECT(strcmp(oid_a, oid_b) == 0);
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);

    /* CE EXIT must not END Critical */
    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 11u, &c));
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &f.om);
    ear_obs_manager_poll(&f.om, 12000000);
    EXPECT(ear_critical_is_active(&f.crit));
    EXPECT(strcmp(oid_a, oid_b) == 0);

    integ_fini(&f);
}

/* ========== Test 5 ========== */

static void test5_ota_cleanup_while_critical_active(void)
{
    printf("test5_ota_cleanup_while_critical_active\n");

    /* 5a: Critical alone (empty registry) — cleanup must not need slots */
    {
        integ_t f;
        integ_init(&f);
        EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);

        ae_signature_t sig = make_impulse_sig(1);
        EXPECT(ear_critical_force_start(&f.crit, &sig, 1000000));
        EXPECT(ear_critical_is_active(&f.crit));
        char oid_pre[EAR_CRITICAL_OBS_ID_LEN];
        EXPECT(ear_critical_active_observation_id(&f.crit, oid_pre));

        /* Production OTA product-binding release (same helper as device close). */
        ear_obs_ota_release_product_bindings(&f.om, &f.crit, 2000000);

        EXPECT(!ear_critical_is_active(&f.crit));
        ear_critical_diag_t d;
        ear_critical_get_diag(&f.crit, &d);
        EXPECT(d.end_ota == 1u);
        EXPECT(g_crit.ends >= 1u);
        EXPECT(strcmp(g_crit.last_term, "ota_reboot") == 0);
        EXPECT(strcmp(g_crit.last_origin, EAR_CRITICAL_ORIGIN) == 0);
        EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);

        /*
         * Reboot boundary (host simulation): re-init Critical Path as a fresh
         * boot would. Does not prove physical reboot / BSS clear on device.
         */
        ear_critical_config_t ccfg;
        ear_critical_config_set_defaults(&ccfg);
        ccfg.auto_detect_enabled = false;
        ccfg.publish = crit_publish;
        ccfg.uuid_fn = crit_uuid;
        EXPECT(ear_critical_init(&f.crit, &ccfg));
        EXPECT(!ear_critical_is_active(&f.crit));

        ae_signature_t sig_new = make_impulse_sig(2);
        EXPECT(ear_critical_force_start(&f.crit, &sig_new, 3000000));
        char oid_post[EAR_CRITICAL_OBS_ID_LEN];
        EXPECT(ear_critical_active_observation_id(&f.crit, oid_post));
        EXPECT(strcmp(oid_pre, oid_post) != 0);

        integ_fini(&f);
    }

    /* 5b: Critical + both normal slots — OTA releases Critical; registry
     * slot teardown remains the device OTA loop (not claimed here). Product
     * binding helper must still terminate Critical without touching slot
     * observation_id storage until registry release runs. */
    {
        integ_t f;
        integ_init(&f);
        plant(&f.ce, 21u, AE_CAND_ACTIVE, 5u, -30.0f);
        plant(&f.ce, 22u, AE_CAND_ACTIVE, 5u, -31.0f);
        EXPECT(promote_with_reserve(&f, 21u, 1000000));
        EXPECT(promote_with_reserve(&f, 22u, 2000000));
        EXPECT(sp_episode_registry_in_use_count(&f.reg) == 2u);

        bool before_use[SP_OBS_EPISODE_SLOTS];
        char before_ids[SP_OBS_EPISODE_SLOTS][SP_OBS_EPISODE_ID_LEN];
        uint16_t before_gen[SP_OBS_EPISODE_SLOTS];
        snapshot_registry(&f.reg, before_use, before_ids, before_gen);

        ae_signature_t sig = make_impulse_sig(30);
        EXPECT(ear_critical_force_start(&f.crit, &sig, 3000000));
        EXPECT(ear_critical_is_active(&f.crit));

        ear_obs_ota_release_product_bindings(&f.om, &f.crit, 4000000);

        EXPECT(!ear_critical_is_active(&f.crit));
        ear_critical_diag_t d;
        ear_critical_get_diag(&f.crit, &d);
        EXPECT(d.end_ota >= 1u);
        EXPECT(strcmp(g_crit.last_term, "ota_reboot") == 0);

        /* forget_bindings clears manager table; registry slots still in_use
         * until device OTA loop releases them — assert ids unchanged here. */
        EXPECT(sp_episode_registry_in_use_count(&f.reg) == 2u);
        bool after_use[SP_OBS_EPISODE_SLOTS];
        char after_ids[SP_OBS_EPISODE_SLOTS][SP_OBS_EPISODE_ID_LEN];
        uint16_t after_gen[SP_OBS_EPISODE_SLOTS];
        snapshot_registry(&f.reg, after_use, after_ids, after_gen);
        for (uint8_t i = 0; i < SP_OBS_EPISODE_SLOTS; ++i) {
            EXPECT(after_use[i] == before_use[i]);
            EXPECT(strcmp(after_ids[i], before_ids[i]) == 0);
            EXPECT(after_gen[i] == before_gen[i]);
        }
        EXPECT(ear_obs_manager_n_open(&f.om) == 0u);

        integ_fini(&f);
    }

    /* 5c: NULL manager — Critical-only OTA release */
    {
        ear_critical_path_t cp;
        memset(&g_crit, 0, sizeof(g_crit));
        ear_critical_config_t ccfg;
        ear_critical_config_set_defaults(&ccfg);
        ccfg.publish = crit_publish;
        ccfg.uuid_fn = crit_uuid;
        EXPECT(ear_critical_init(&cp, &ccfg));
        ae_signature_t sig = make_impulse_sig(40);
        EXPECT(ear_critical_force_start(&cp, &sig, 1000));
        ear_obs_ota_release_product_bindings(NULL, &cp, 2000);
        EXPECT(!ear_critical_is_active(&cp));
        EXPECT(strcmp(g_crit.last_term, "ota_reboot") == 0);
    }
}

int main(void)
{
    test4_critical_with_both_normal_slots_occupied();
    test5_ota_cleanup_while_critical_active();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
