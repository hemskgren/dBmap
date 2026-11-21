/**
 * Integration host tests: Critical Path vs normal Obs slots + OTA cleanup.
 *
 * Test 4 — Critical with both normal Observation slots occupied.
 * Test 5 — OTA product-binding cleanup while Critical is active (+ boot boundary).
 * Package 3A — AE listener triad (SE → CE → Critical) + Critical coexistence.
 *
 * Uses production ear_obs_manager, episode registry, ear_critical_path, and
 * ear_obs_ota_release_product_bindings (same helper as ear_observation_close_for_ota).
 *
 * Automatic acoustic detection remains disabled (force_start only).
 *
 * Preferred: included in ./firmware/scripts/run_firmware_fast_gate.sh (--host-only).
 *
 * Standalone from repo root (keep includes aligned with the fast gate):
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/platform/audio/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     -I firmware/platform/mic/include \
 *     -I firmware/platform/node_common/include \
 *     -I firmware/products/dbmap/ear/candidate_engine/include \
 *     -I firmware/products/dbmap/ear/acoustic_profile/include \
 *     -I firmware/products/dbmap/ear/signature_engine/include \
 *     -I firmware/products/dbmap/ear/ear_observation/include \
 *     -I firmware/platform/observation/include \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_history.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_engine.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_time.c \
 *     firmware/products/dbmap/ear/acoustic_profile/as_profile_as001.c \
 *     firmware/products/dbmap/ear/acoustic_profile/as_profile_registry.c \
 *     firmware/products/dbmap/ear/acoustic_profile/as_profile_match.c \
 *     firmware/products/dbmap/ear/signature_engine/sig_engine.c \
 *     firmware/products/dbmap/ear/signature_engine/ear_siren_se_public.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_decision.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_module.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_module_profile.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_workflow.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_manager.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_engine.c \
 *     firmware/platform/observation/episode_registry.c \
 *     firmware/products/dbmap/ear/ear_observation/ear_cls_progressive.c \
 *     firmware/products/dbmap/ear/ear_observation/ear_cls_as001.c \
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
#include "sig_engine.h"
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
    int64_t last_start_mono_us;
    int64_t last_end_mono_us;
    bool have_start_ts;
    bool have_end_ts;
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
    g_norm.starts++;
    g_norm.last_start_mono_us = now_mono_us;
    g_norm.have_start_ts = true;
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
    (void)has_lvl;
    (void)lvl;
    g_norm.ends++;
    g_norm.last_end_mono_us = b;
    g_norm.have_end_ts = true;
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
    int64_t last_start_capture_mono_us;
    int64_t last_end_capture_mono_us;
    bool have_start_ts;
    bool have_end_ts;
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
    (void)interval_duration_ms;
    (void)episode_duration_ms;
    (void)evidence_or_null;
    if (strcmp(emission_kind, "start") == 0) {
        g_crit.starts++;
        g_crit.last_start_capture_mono_us = capture_mono_us;
        g_crit.have_start_ts = true;
    } else if (strcmp(emission_kind, "periodic") == 0) {
        g_crit.periodics++;
    } else if (strcmp(emission_kind, "end") == 0) {
        g_crit.ends++;
        g_crit.last_end_capture_mono_us = capture_mono_us;
        g_crit.have_end_ts = true;
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

/**
 * Product-style Critical AE listener (mirrors ear_observation.c
 * critical_on_ae_signature). Uses signature window end as host mono clock.
 */
static void critical_ae_listener(const ae_signature_t *sig, void *ctx)
{
    ear_critical_path_t *cp = (ear_critical_path_t *)ctx;
    if (cp == NULL || sig == NULL) {
        return;
    }
    ear_critical_on_signature(cp, sig, sig->window_end_mono_us);
}

/**
 * Production-style product poll after AE publish (ear_observation.c activity
 * path). Order is observable: Policy must run before OM for same-cycle START.
 * Host mirrors call order; FreeRTOS scheduling is not verified here.
 */
static void p3a_drive_poll(
    ear_promotion_policy_t *pol,
    ear_obs_manager_t *om,
    ce_engine_t *ce,
    ear_critical_path_t *crit,
    int64_t now_mono_us
)
{
    ear_promotion_policy_poll(pol, now_mono_us);
    ear_obs_manager_poll(om, now_mono_us);
    ear_obs_manager_feed_from_ce(om, ce);
    ear_critical_poll(crit, now_mono_us);
}

static int find_open_bind_idx(const ear_obs_manager_t *om, uint64_t cand_id)
{
    if (om == NULL) {
        return -1;
    }
    for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (om->bind[i].in_use && om->bind[i].candidate_id == cand_id) {
            return (int)i;
        }
    }
    return -1;
}

/* Package 2 synthetic signature fixtures (short smoke reuse; not full lifecycle). */
static void p3a_fill_heli_sig(ae_signature_t *s, uint32_t seq, float rms_dbfs)
{
    memset(s, 0, sizeof(*s));
    s->schema_version = AE_SIGNATURE_SCHEMA_VERSION;
    s->window_seq = seq;
    s->window_end_mono_us = (int64_t)seq * 1000000LL;
    s->window_ms = 1000u;
    s->mic_mask = 0x1u;
    s->quality = AE_QUALITY_OK;
    s->flags = AE_FLAG_PAIR_MISSING;
    s->rms_dbfs = rms_dbfs;
    s->peak_dbfs = rms_dbfs + 7.3f;
    s->crest_db = 7.3f;
    s->band_dbfs[0] = rms_dbfs - 4.21f;
    s->band_dbfs[1] = rms_dbfs - 3.52f;
    s->band_dbfs[2] = rms_dbfs - 13.10f;
    s->band_dbfs[3] = rms_dbfs - 32.77f;
    s->hl_valid = true;
    s->spectral_valid = true;
    s->dominant_hz = 230.0f;
    s->flatness = 0.141f;
    s->centroid_hz = 800.0f;
    s->bandwidth_hz = 920.0f;
}

static void p3a_fill_broadband_sig(ae_signature_t *s, uint32_t seq, float rms_dbfs)
{
    memset(s, 0, sizeof(*s));
    s->schema_version = AE_SIGNATURE_SCHEMA_VERSION;
    s->window_seq = seq;
    s->window_end_mono_us = (int64_t)seq * 1000000LL;
    s->window_ms = 1000u;
    s->mic_mask = 0x1u;
    s->quality = AE_QUALITY_OK;
    s->flags = AE_FLAG_PAIR_MISSING;
    s->rms_dbfs = rms_dbfs;
    s->peak_dbfs = rms_dbfs + 8.0f;
    s->crest_db = 8.0f;
    s->band_dbfs[0] = rms_dbfs - 2.0f;
    s->band_dbfs[1] = rms_dbfs - 1.0f;
    s->band_dbfs[2] = rms_dbfs - 1.5f;
    s->band_dbfs[3] = rms_dbfs - 3.0f;
    s->hl_valid = true;
    s->spectral_valid = true;
    s->dominant_hz = 1800.0f;
    s->flatness = 0.70f;
    s->centroid_hz = 2200.0f;
    s->bandwidth_hz = 2500.0f;
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

/* ========== Package 3A ========== */

/**
 * Package 3A — AE listener triad + Critical coexistence (public APIs).
 *
 * Mirrors production wiring in ear_observation.c:
 *   SE listener → CE bind → Critical AE listener (count 3)
 *   Policy → OM → feed_from_ce → critical_poll  (via p3a_drive_poll)
 *
 * Observable evidence (not mere call order):
 *   - ae_listener_count == 3 after triad registration
 *   - Real AS-001 AE publish → SE primary + CE classification + Policy START
 *     through p3a_drive_poll (Policy before OM required for same-cycle START)
 *   - OM owns CE-associated heli evidence (window seq + rms), not just a counter
 *   - Critical force_start: distinct id, origin critical_path, registry unchanged
 *   - AE publish with auto-detect off does not Critical-START
 *   - one monotonic fixture clock across AE/Policy/OM/Critical/cleanup
 *   - normal END timestamp >= START; Critical capture times nondecreasing
 *   - cleanup ends Observations, drains EXIT, listeners → 0, registry → 0
 *
 * Short smoke only — not Package 2's full AS-001 lifecycle.
 */
#define P3A_REQUIRE(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
            goto p3a_cleanup;                                                  \
        }                                                                      \
    } while (0)

static void test_p3a_ae_listener_triad_critical_coexist(void)
{
    printf("test_p3a_ae_listener_triad_critical_coexist\n");

    integ_t f;
    memset(&f, 0, sizeof(f));
    memset(&g_norm, 0, sizeof(g_norm));
    memset(&g_crit, 0, sizeof(g_crit));
    g_uuid_n = 1;

    ear_promotion_policy_t pol;
    memset(&pol, 0, sizeof(pol));

    bool se_bound = false;
    bool crit_bound = false;
    bool ce_started = false;
    bool sig_started = false;
    bool om_ready = false;
    bool crit_force_started = false;
    const uint32_t reg_initial = 0u;
    const uint32_t heli_seq = 17u;
    const float heli_rms = -40.0f;
    /* Single nondecreasing fixture clock for AE/Policy/OM/Critical/cleanup. */
    int64_t now_us = 0;
    int64_t cleanup_now_us = 0;

    ae_config_t acfg;
    ae_config_set_defaults(&acfg);
    P3A_REQUIRE(ae_start(&f.ae, &acfg));

    P3A_REQUIRE(sig_engine_start());
    sig_started = true;
    /* Production order: Signature Engine before CE on the AE listener list. */
    P3A_REQUIRE(ae_add_signature_listener(&f.ae, sig_engine_ae_listener, NULL));
    se_bound = true;
    P3A_REQUIRE(ae_listener_count(&f.ae) == 1u);

    ce_config_t ccfg;
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 1u;
    ccfg.t_coast = 3u;
    P3A_REQUIRE(ce_start(&f.ce, &ccfg));
    ce_started = true;
    P3A_REQUIRE(ce_bind_audio_engine(&f.ce, &f.ae));
    P3A_REQUIRE(ae_listener_count(&f.ae) == 2u);

    sp_episode_registry_init(&f.reg);
    P3A_REQUIRE(sp_episode_registry_in_use_count(&f.reg) == reg_initial);

    ear_obs_manager_config_t mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.registry = &f.reg;
    mcfg.ce = &f.ce;
    mcfg.ae = &f.ae;
    mcfg.uuid_fn = fake_uuid;
    mcfg.observation_interval_ms = 1000u;
    mcfg.pending_timeout_ms = 30000u;
    mcfg.detached_end_timeout_ms = 1000u;
    mcfg.producer_tag = 2u;
    mcfg.emit_start = norm_emit_start;
    mcfg.emit_periodic = norm_emit_periodic;
    mcfg.emit_end = norm_emit_end;
    mcfg.enable_poll_periodic = false;
    ear_obs_manager_init(&f.om, &mcfg);
    om_ready = true;
    ce_set_promotion_cb(&f.ce, ear_obs_manager_on_promotion, &f.om);
    ear_promotion_policy_init(&pol, &f.ce, &f.om);

    ear_critical_config_t ccfg_c;
    ear_critical_config_set_defaults(&ccfg_c);
    ccfg_c.auto_detect_enabled = false;
    ccfg_c.lab_impulse_enabled = false;
    ccfg_c.publish = crit_publish;
    ccfg_c.uuid_fn = crit_uuid;
    ccfg_c.periodic_ms = 5000u;
    ccfg_c.idle_timeout_ms = 15000u;
    ccfg_c.max_active_ms = 30000u;
    P3A_REQUIRE(ear_critical_init(&f.crit, &ccfg_c));
    /* Production order: Critical AE listener after SE and CE. */
    P3A_REQUIRE(ae_add_signature_listener(&f.ae, critical_ae_listener, &f.crit));
    crit_bound = true;
    P3A_REQUIRE(ae_listener_count(&f.ae) == 3u);

    ae_signature_t sig;

    /* Unmatched broadband: SE/CE run, Policy must not open an Observation. */
    for (uint32_t seq = 1u; seq <= 4u; seq++) {
        p3a_fill_broadband_sig(&sig, seq, -55.0f);
        now_us = sig.window_end_mono_us;
        cleanup_now_us = now_us;
        P3A_REQUIRE(ae_publish_signature(&f.ae, &sig));
        p3a_drive_poll(&pol, &f.om, &f.ce, &f.crit, now_us);
    }
    P3A_REQUIRE(g_norm.starts == 0u);
    P3A_REQUIRE(ear_obs_manager_n_open(&f.om) == 0u);
    P3A_REQUIRE(sp_episode_registry_in_use_count(&f.reg) == reg_initial);

    /*
     * Real AS-001 heli through AE listeners, then p3a_drive_poll for Policy START.
     * Assert SE/CE dispatch from the publish itself (before poll), so suppressing
     * either listener fails for that reason — not only a missing START.
     */
    p3a_fill_heli_sig(&sig, heli_seq, heli_rms);
    now_us = sig.window_end_mono_us;
    cleanup_now_us = now_us;
    P3A_REQUIRE(ae_publish_signature(&f.ae, &sig));

    /* SE processed the current window (fails if SE AE dispatch is suppressed). */
    sig_primary_distance_t prim;
    memset(&prim, 0, sizeof(prim));
    P3A_REQUIRE(sig_engine_get_primary_distance_match(&prim));
    P3A_REQUIRE(prim.valid);
    P3A_REQUIRE(prim.window_seq == heli_seq);
    P3A_REQUIRE(prim.profile_id == AS_PROFILE_ID_AS001);

    /*
     * CE ingested the published heli (fails if CE AE dispatch is suppressed).
     * With SE started, as.profile uses SE primary for this window.
     */
    uint64_t x_id = 0u;
    {
        bool found = false;
        for (uint8_t i = 0u; i < CE_MAX_CANDIDATES; i++) {
            ae_candidate_t c;
            if (!ce_get_candidate(&f.ce, i, &c)) {
                continue;
            }
            if (c.last_window_seq == heli_seq) {
                x_id = c.candidate_id;
                found = true;
                break;
            }
        }
        P3A_REQUIRE(found && x_id != 0u);
    }
    ae_candidate_t live;
    memset(&live, 0, sizeof(live));
    P3A_REQUIRE(ce_find_candidate(&f.ce, x_id, &live));
    P3A_REQUIRE(live.last_window_seq == heli_seq);
    ce_classification_t cls;
    memset(&cls, 0, sizeof(cls));
    P3A_REQUIRE(ce_get_classification(&f.ce, x_id, &cls));
    P3A_REQUIRE(cls.valid && cls.matched);
    P3A_REQUIRE(cls.profile_id == AS_PROFILE_ID_AS001);
    P3A_REQUIRE(cls.distance == prim.distance);

    /*
     * Same-cycle START via p3a_drive_poll: Policy before OM is required.
     * Reversing those two calls inside the helper leaves starts == 0.
     */
    p3a_drive_poll(&pol, &f.om, &f.ce, &f.crit, now_us);
    P3A_REQUIRE(g_norm.starts == 1u);
    P3A_REQUIRE(g_norm.have_start_ts);
    P3A_REQUIRE(g_norm.last_start_mono_us == now_us);
    P3A_REQUIRE(ear_obs_manager_n_open(&f.om) == 1u);
    P3A_REQUIRE(sp_episode_registry_in_use_count(&f.reg) == 1u);
    P3A_REQUIRE(g_norm.ids[0][0] != '\0');
    P3A_REQUIRE(g_norm.n_ids >= 1u);

    P3A_REQUIRE(ce_find_candidate(&f.ce, x_id, &live));
    P3A_REQUIRE(live.state == AE_CAND_PROMOTED);
    P3A_REQUIRE(live.promotion_entered);

    {
        ear_promotion_policy_diag_t pd;
        ear_promotion_policy_get_diag(&pol, &pd);
        P3A_REQUIRE(pd.promote_ok >= 1u);
        P3A_REQUIRE(pd.last_decision == EAR_PROMO_DECISION_PROMOTE);
        P3A_REQUIRE(pd.last_profile_id == AS_PROFILE_ID_AS001);
    }

    const uint32_t reg_after_normal = sp_episode_registry_in_use_count(&f.reg);
    const int bi = find_open_bind_idx(&f.om, x_id);
    P3A_REQUIRE(bi >= 0);
    P3A_REQUIRE((uint8_t)bi < EAR_OBS_MGR_BINDING_CAP);
    P3A_REQUIRE(f.om.bind[bi].in_use);
    P3A_REQUIRE(f.om.bind[bi].ce_live);
    P3A_REQUIRE(strcmp(f.om.bind[bi].observation_id, g_norm.ids[0]) == 0);
    /*
     * CE-associated owned evidence from feed_from_ce in p3a_drive_poll.
     * Sequence + rms pin the heli window (not evidence_updates alone).
     */
    P3A_REQUIRE(f.om.bind[bi].evidence_updates >= 1u);
    P3A_REQUIRE(f.om.bind[bi].snap.sig_count >= 1u);
    P3A_REQUIRE(f.om.bind[bi].last_owned_sig_window_seq == heli_seq);
    P3A_REQUIRE(f.om.bind[bi].snap.sigs[0].window_seq == heli_seq);
    P3A_REQUIRE(f.om.bind[bi].snap.sigs[0].quality == AE_QUALITY_OK);
    P3A_REQUIRE(f.om.bind[bi].snap.sigs[0].rms_dbfs == heli_rms);

    /*
     * Heli AE publish already reached Critical while idle: production
     * auto_detect/lab off → auto_start_blocked, no Critical START.
     */
    {
        ear_critical_diag_t cd;
        ear_critical_get_diag(&f.crit, &cd);
        P3A_REQUIRE(cd.auto_start_blocked >= 1u);
    }
    P3A_REQUIRE(g_crit.starts == 0u);
    P3A_REQUIRE(!ear_critical_is_active(&f.crit));
    P3A_REQUIRE(ear_obs_manager_n_open(&f.om) == 1u);

    now_us += 1000000LL;
    cleanup_now_us = now_us;
    ae_signature_t crit_sig = make_impulse_sig(60);
    crit_sig.window_end_mono_us = now_us;
    P3A_REQUIRE(ear_critical_force_start(&f.crit, &crit_sig, now_us));
    crit_force_started = true;
    P3A_REQUIRE(ear_critical_is_active(&f.crit));
    P3A_REQUIRE(g_crit.starts == 1u);
    P3A_REQUIRE(g_crit.have_start_ts);
    P3A_REQUIRE(g_crit.last_start_capture_mono_us == now_us);
    P3A_REQUIRE(strcmp(g_crit.last_origin, EAR_CRITICAL_ORIGIN) == 0);
    P3A_REQUIRE(strcmp(g_crit.last_origin, "critical_path") == 0);

    char crit_oid[EAR_CRITICAL_OBS_ID_LEN];
    P3A_REQUIRE(ear_critical_active_observation_id(&f.crit, crit_oid));
    P3A_REQUIRE(strcmp(crit_oid, g_norm.ids[0]) != 0);
    P3A_REQUIRE(sp_episode_registry_in_use_count(&f.reg) == reg_after_normal);
    P3A_REQUIRE(ear_obs_manager_n_open(&f.om) == 1u);

    /* Publish while Critical active: no additional Critical START; PERIODIC via poll. */
    const uint32_t crit_starts_before = g_crit.starts;
    /* periodic_ms is 5000 — advance fixture clock enough for one PERIODIC. */
    now_us += 5000000LL;
    cleanup_now_us = now_us;
    ae_signature_t live_sig = make_impulse_sig(70);
    live_sig.window_end_mono_us = now_us;
    P3A_REQUIRE(ae_publish_signature(&f.ae, &live_sig));
    p3a_drive_poll(&pol, &f.om, &f.ce, &f.crit, now_us);
    P3A_REQUIRE(g_crit.starts == crit_starts_before);
    P3A_REQUIRE(ear_critical_is_active(&f.crit));
    P3A_REQUIRE(g_crit.periodics >= 1u);
    P3A_REQUIRE(strcmp(g_crit.last_origin, EAR_CRITICAL_ORIGIN) == 0);
    P3A_REQUIRE(sp_episode_registry_in_use_count(&f.reg) == reg_after_normal);
    P3A_REQUIRE(ear_obs_manager_n_open(&f.om) == 1u);
    /* Cleanup must run after the latest scenario event (not before START). */
    cleanup_now_us = now_us + 1000000LL;

p3a_cleanup:
    /*
     * Lifecycle cleanup before engine/listener teardown. Idempotent on early
     * abort: force-end only when something is active/open.
     */
    if (crit_force_started || ear_critical_is_active(&f.crit)) {
        ear_critical_force_end(&f.crit, cleanup_now_us, EAR_CRITICAL_END_EXPLICIT);
        crit_force_started = false;
    }
    if (om_ready) {
        if (ear_obs_manager_n_open(&f.om) > 0u
            || ear_obs_manager_n_pending(&f.om) > 0u) {
            ear_obs_manager_force_end_all(
                &f.om, EAR_OBS_END_EXPLICIT, cleanup_now_us + 1000
            );
        }
        /* Drain EXIT/pending transitions before ce_stop removes candidates. */
        ear_obs_manager_poll(&f.om, cleanup_now_us + 2000);
    }
    if (crit_bound) {
        (void)ae_remove_signature_listener(&f.ae, critical_ae_listener, &f.crit);
        crit_bound = false;
    }
    if (ce_started) {
        ce_set_promotion_cb(&f.ce, NULL, NULL);
        ce_stop(&f.ce);
        ce_started = false;
        if (om_ready) {
            /* Drain any EXIT queued by ce_stop. */
            ear_obs_manager_poll(&f.om, cleanup_now_us + 3000);
            if (ear_obs_manager_n_open(&f.om) > 0u) {
                ear_obs_manager_force_end_all(
                    &f.om, EAR_OBS_END_EXPLICIT, cleanup_now_us + 4000
                );
                ear_obs_manager_poll(&f.om, cleanup_now_us + 5000);
            }
        }
    }
    if (se_bound) {
        (void)ae_remove_signature_listener(&f.ae, sig_engine_ae_listener, NULL);
        se_bound = false;
    }
    if (sig_started) {
        sig_engine_stop();
        sig_started = false;
    }

    if (ear_critical_is_active(&f.crit)) {
        fprintf(stderr, "FAIL %s:%d: Critical still active after cleanup\n",
                __FILE__, __LINE__);
        g_failures++;
    }
    if (om_ready && ear_obs_manager_n_open(&f.om) != 0u) {
        fprintf(stderr, "FAIL %s:%d: normal OM still open after cleanup (n=%u)\n",
                __FILE__, __LINE__, (unsigned)ear_obs_manager_n_open(&f.om));
        g_failures++;
    }
    if (om_ready && ear_obs_manager_n_pending(&f.om) != 0u) {
        fprintf(stderr, "FAIL %s:%d: normal OM still pending after cleanup\n",
                __FILE__, __LINE__);
        g_failures++;
    }
    if (sp_episode_registry_in_use_count(&f.reg) != reg_initial) {
        fprintf(
            stderr,
            "FAIL %s:%d: registry occupancy after cleanup expected %u got %u\n",
            __FILE__,
            __LINE__,
            (unsigned)reg_initial,
            (unsigned)sp_episode_registry_in_use_count(&f.reg)
        );
        g_failures++;
    }
    if (ae_listener_count(&f.ae) != 0u) {
        fprintf(
            stderr,
            "FAIL %s:%d: ae_listener_count cleanup expected 0 got %u\n",
            __FILE__,
            __LINE__,
            (unsigned)ae_listener_count(&f.ae)
        );
        g_failures++;
    }
    /* Opened normal Observation must END at/after its START (fixture clock). */
    if (g_norm.starts >= 1u) {
        if (g_norm.ends != 1u) {
            fprintf(
                stderr,
                "FAIL %s:%d: expected exactly 1 normal END after START, got %u\n",
                __FILE__,
                __LINE__,
                (unsigned)g_norm.ends
            );
            g_failures++;
        }
        if (!g_norm.have_start_ts || !g_norm.have_end_ts) {
            fprintf(
                stderr,
                "FAIL %s:%d: missing normal START/END timestamps "
                "(have_start=%d have_end=%d)\n",
                __FILE__,
                __LINE__,
                (int)g_norm.have_start_ts,
                (int)g_norm.have_end_ts
            );
            g_failures++;
        } else if (g_norm.last_end_mono_us < g_norm.last_start_mono_us) {
            fprintf(
                stderr,
                "FAIL %s:%d: normal END before START "
                "(start=%lld end=%lld)\n",
                __FILE__,
                __LINE__,
                (long long)g_norm.last_start_mono_us,
                (long long)g_norm.last_end_mono_us
            );
            g_failures++;
        }
    }
    if (g_crit.starts >= 1u) {
        if (g_crit.ends < 1u || !g_crit.have_start_ts || !g_crit.have_end_ts) {
            fprintf(
                stderr,
                "FAIL %s:%d: Critical START without valid END timestamps "
                "(ends=%u have_start=%d have_end=%d)\n",
                __FILE__,
                __LINE__,
                (unsigned)g_crit.ends,
                (int)g_crit.have_start_ts,
                (int)g_crit.have_end_ts
            );
            g_failures++;
        } else if (g_crit.last_end_capture_mono_us
                   < g_crit.last_start_capture_mono_us) {
            fprintf(
                stderr,
                "FAIL %s:%d: Critical END capture before START "
                "(start=%lld end=%lld)\n",
                __FILE__,
                __LINE__,
                (long long)g_crit.last_start_capture_mono_us,
                (long long)g_crit.last_end_capture_mono_us
            );
            g_failures++;
        }
    }
}
#undef P3A_REQUIRE

int main(void)
{
    test4_critical_with_both_normal_slots_occupied();
    test5_ota_cleanup_while_critical_active();
    test_p3a_ae_listener_triad_critical_coexist();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
