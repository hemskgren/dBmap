/**
 * Integration host tests: Critical Path vs normal Obs slots + OTA cleanup.
 *
 * Test 4 — Critical with both normal Observation slots occupied.
 * Test 5 — OTA product-binding cleanup while Critical is active (+ boot boundary).
 * Package 3A — AE listener triad (SE → CE → Critical) + Critical coexistence.
 * Package 4 — AS-001/AS-002 coexistence through the production SE listener
 * and ear_sig_product_poll (Critical stays a separate listener/poll).
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
 *     firmware/products/dbmap/ear/ear_observation/ear_sig_product.c \
 *     firmware/products/dbmap/ear/ear_observation/ear_critical_path.c \
 *     firmware/products/dbmap/ear/ear_observation/ear_obs_ota_cleanup.c \
 *     firmware/products/dbmap/ear/ear_observation/test_host/test_ear_critical_integration.c \
 *     -lm -o /tmp/test_ear_critical_integration && /tmp/test_ear_critical_integration
 *
 * Host / source-level only — not physical reboot or Hub MQTT verification.
 *
 * Publish-contract export (separate link: observation_payload.c, cJSON,
 * identity.c, -DDBMAP_OBS_PUBLISH_CONTRACT_EXPORT):
 *   ./test_ear_critical_integration --export-publish-contract /tmp/dir
 * Writes report-01.json .. report-07.json and topic.txt. Without that
 * flag, or without the define, main runs the existing host tests.
 */
#include "ae_engine.h"
#include "ae_signature.h"
#include "as_profile.h"
#include "ce_engine.h"
#include "ear_critical_path.h"
#include "ear_obs_manager.h"
#include "ear_obs_ota_cleanup.h"
#include "ear_promotion_policy.h"
#include "ear_sig_product.h"
#include "sig_engine.h"
#include "sp_episode_registry.h"

#ifdef DBMAP_OBS_PUBLISH_CONTRACT_EXPORT
#include "dbmap_observation_payload.h"
#include "sp_types.h"

#include <stdlib.h>
#endif

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#ifdef DBMAP_OBS_PUBLISH_CONTRACT_EXPORT
static void contract_note_normal(
    const char *kind,
    const ear_obs_binding_view_t *binding,
    const ear_obs_promote_snapshot_t *snap,
    int64_t interval_start_mono_us,
    int64_t capture_mono_us,
    bool has_level,
    float level_dbfs
);
static void contract_note_critical(
    const char *observation_id,
    const char *emission_kind,
    const char *termination_reason,
    const char *origin,
    int64_t interval_start_mono_us,
    int64_t capture_mono_us,
    double interval_duration_ms,
    double episode_duration_ms
);
#endif

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
    char last_end_id[SP_OBS_EPISODE_ID_LEN];
    int64_t last_start_mono_us;
    int64_t last_end_mono_us;
    uint16_t start_generation;
    uint16_t end_generation;
    bool have_start_ts;
    bool have_end_ts;
    bool have_end_id;
    bool have_start_gen;
    bool have_end_gen;
} normal_emit_t;

static normal_emit_t g_norm;

/* Package 4 emit capture. Inactive unless the coexistence test turns it on. */
#define P4_EMIT_CAP 16u

typedef struct {
    char id[SP_OBS_EPISODE_ID_LEN];
    char path[96];
    bool start;
    bool periodic;
} p4_emit_rec_t;

static bool g_p4_capture;
static p4_emit_rec_t g_p4_emits[P4_EMIT_CAP];
static uint32_t g_p4_emit_n;
static ear_obs_end_reason_t g_p4_last_end_reason;
static char g_p4_last_end_id[SP_OBS_EPISODE_ID_LEN];
static int64_t g_p4_last_end_capture_mono_us;

static void p4_assess_path(
    const ear_cls_assessment_wire_t *w,
    char *out,
    size_t cap
)
{
    if (out == NULL || cap == 0u) {
        return;
    }
    out[0] = '\0';
    if (w == NULL || !w->present || w->n_nodes == 0u) {
        return;
    }
    size_t used = 0u;
    const uint8_t n =
        w->n_nodes < EAR_CLS_PATH_MAX_NODES ? w->n_nodes : EAR_CLS_PATH_MAX_NODES;
    for (uint8_t i = 0u; i < n; i++) {
        const char *id = w->nodes[i].id;
        const size_t idn = strlen(id);
        const size_t slash = used > 0u ? 1u : 0u;
        if (used + slash + idn + 1u > cap) {
            out[0] = '\0';
            return;
        }
        if (slash != 0u) {
            out[used++] = '/';
        }
        memcpy(out + used, id, idn);
        used += idn;
        out[used] = '\0';
    }
}

static bool p4_path_is(const ear_cls_assessment_wire_t *w, const char *expect)
{
    char got[96];
    p4_assess_path(w, got, sizeof(got));
    return expect != NULL && strcmp(got, expect) == 0;
}

static void p4_note_emit(
    const ear_obs_binding_view_t *binding,
    bool start,
    bool periodic
)
{
    if (!g_p4_capture || binding == NULL || g_p4_emit_n >= P4_EMIT_CAP) {
        return;
    }
    p4_emit_rec_t *rec = &g_p4_emits[g_p4_emit_n++];
    memset(rec, 0, sizeof(*rec));
    snprintf(rec->id, sizeof(rec->id), "%s", binding->observation_id);
    p4_assess_path(&binding->assessment, rec->path, sizeof(rec->path));
    rec->start = start;
    rec->periodic = periodic;
}

static bool p4_emit_path_is(const char *obs_id, bool start, const char *path)
{
    if (obs_id == NULL || path == NULL) {
        return false;
    }
    for (uint32_t i = 0u; i < g_p4_emit_n; i++) {
        const p4_emit_rec_t *rec = &g_p4_emits[i];
        if (rec->start != start || rec->periodic == start) {
            continue;
        }
        if (strcmp(rec->id, obs_id) == 0 && strcmp(rec->path, path) == 0) {
            return true;
        }
    }
    return false;
}

static void p3a_copy_id(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_len, "%s", src);
}

static bool norm_emit_start(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    const ear_obs_promote_snapshot_t *snap,
    int64_t now_mono_us
)
{
    (void)ctx;
    g_norm.starts++;
#ifdef DBMAP_OBS_PUBLISH_CONTRACT_EXPORT
    contract_note_normal("start", binding, snap, now_mono_us, now_mono_us, false, 0.0f);
#else
    (void)snap;
#endif
    g_norm.last_start_mono_us = now_mono_us;
    g_norm.have_start_ts = true;
    if (binding == NULL) {
        return true;
    }
    g_norm.start_generation = binding->ep.generation;
    g_norm.have_start_gen = true;
    p4_note_emit(binding, true, false);
    if (g_norm.n_ids < SP_OBS_EPISODE_SLOTS) {
        p3a_copy_id(
            g_norm.ids[g_norm.n_ids],
            sizeof(g_norm.ids[g_norm.n_ids]),
            binding->observation_id
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
    g_norm.periodics++;
#ifdef DBMAP_OBS_PUBLISH_CONTRACT_EXPORT
    contract_note_normal("periodic", binding, NULL, a, b, has_lvl, lvl);
#else
    (void)binding;
    (void)a;
    (void)b;
    (void)has_lvl;
    (void)lvl;
#endif
    p4_note_emit(binding, false, true);
    return true;
}

static bool norm_emit_end(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    ear_obs_end_reason_t reason,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool has_lvl,
    float lvl
)
{
    (void)ctx;
    (void)interval_start_mono_us;
    (void)has_lvl;
    (void)lvl;
    g_norm.ends++;
    if (g_p4_capture) {
        g_p4_last_end_reason = reason;
        g_p4_last_end_capture_mono_us = interval_end_mono_us;
        if (binding != NULL) {
            snprintf(
                g_p4_last_end_id,
                sizeof(g_p4_last_end_id),
                "%s",
                binding->observation_id
            );
        }
    }
    g_norm.last_end_mono_us = interval_end_mono_us;
    g_norm.have_end_ts = true;
    if (binding == NULL) {
        return true;
    }
    p3a_copy_id(g_norm.last_end_id, sizeof(g_norm.last_end_id), binding->observation_id);
    g_norm.have_end_id = true;
    g_norm.end_generation = binding->ep.generation;
    g_norm.have_end_gen = true;
    return true;
}

/* ---- Critical publish log ---- */

typedef struct {
    uint32_t starts;
    uint32_t periodics;
    uint32_t ends;
    char last_oid[EAR_CRITICAL_OBS_ID_LEN];
    char start_oid[EAR_CRITICAL_OBS_ID_LEN];
    char end_oid[EAR_CRITICAL_OBS_ID_LEN];
    char periodic_oid[EAR_CRITICAL_OBS_ID_LEN];
    char last_kind[16];
    char last_term[16];
    char last_origin[32];
    int64_t last_start_capture_mono_us;
    int64_t periodic_capture_mono_us;
    int64_t last_end_capture_mono_us;
    bool have_start_ts;
    bool have_periodic_ts;
    bool have_end_ts;
    bool have_start_oid;
    bool have_end_oid;
    bool have_periodic_oid;
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
    (void)evidence_or_null;
#ifdef DBMAP_OBS_PUBLISH_CONTRACT_EXPORT
    contract_note_critical(
        observation_id,
        emission_kind,
        termination_reason,
        origin,
        interval_start_mono_us,
        capture_mono_us,
        interval_duration_ms,
        episode_duration_ms
    );
#else
    (void)interval_start_mono_us;
    (void)interval_duration_ms;
    (void)episode_duration_ms;
#endif
    /*
     * Count every START/PERIODIC/END. Keep the first id and capture time.
     * A later event must not replace that record; the count still shows it.
     */
    if (emission_kind != NULL && strcmp(emission_kind, "start") == 0) {
        g_crit.starts++;
        if (!g_crit.have_start_ts) {
            g_crit.last_start_capture_mono_us = capture_mono_us;
            g_crit.have_start_ts = true;
        }
        if (!g_crit.have_start_oid) {
            p3a_copy_id(g_crit.start_oid, sizeof(g_crit.start_oid), observation_id);
            g_crit.have_start_oid = true;
        }
    } else if (emission_kind != NULL && strcmp(emission_kind, "periodic") == 0) {
        g_crit.periodics++;
        if (!g_crit.have_periodic_oid) {
            p3a_copy_id(
                g_crit.periodic_oid,
                sizeof(g_crit.periodic_oid),
                observation_id
            );
            g_crit.have_periodic_oid = true;
        }
        if (!g_crit.have_periodic_ts) {
            g_crit.periodic_capture_mono_us = capture_mono_us;
            g_crit.have_periodic_ts = true;
        }
    } else if (emission_kind != NULL && strcmp(emission_kind, "end") == 0) {
        g_crit.ends++;
        if (!g_crit.have_end_ts) {
            g_crit.last_end_capture_mono_us = capture_mono_us;
            g_crit.have_end_ts = true;
        }
        if (!g_crit.have_end_oid) {
            p3a_copy_id(g_crit.end_oid, sizeof(g_crit.end_oid), observation_id);
            g_crit.have_end_oid = true;
        }
    }
    p3a_copy_id(g_crit.last_oid, sizeof(g_crit.last_oid), observation_id);
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
 *   - normal END id and episode generation match START; END time >= START
 *   - Critical END id matches published START id; capture times nondecreasing
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
    P3A_REQUIRE(g_crit.have_start_oid);
    P3A_REQUIRE(g_crit.start_oid[0] != '\0');
    P3A_REQUIRE(strcmp(g_crit.start_oid, crit_oid) == 0);
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
    P3A_REQUIRE(g_crit.periodics == 1u);
    P3A_REQUIRE(g_crit.have_periodic_oid);
    P3A_REQUIRE(strcmp(g_crit.periodic_oid, g_crit.start_oid) == 0);
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
    /* Opened normal Observation must END once, for the same id, at/after START. */
    if (g_norm.starts != 0u || g_norm.ends != 0u) {
        if (g_norm.starts != 1u || g_norm.ends != 1u) {
            fprintf(
                stderr,
                "FAIL %s:%d: expected exactly 1 normal START and 1 END, got start=%u end=%u\n",
                __FILE__,
                __LINE__,
                (unsigned)g_norm.starts,
                (unsigned)g_norm.ends
            );
            g_failures++;
        }
        if (!g_norm.have_end_id || g_norm.n_ids != 1u
            || g_norm.ids[0][0] == '\0'
            || strcmp(g_norm.last_end_id, g_norm.ids[0]) != 0) {
            fprintf(
                stderr,
                "FAIL %s:%d: normal END observation_id does not match START "
                "(start='%s' end='%s' have_end_id=%d n_ids=%u)\n",
                __FILE__,
                __LINE__,
                g_norm.n_ids >= 1u ? g_norm.ids[0] : "",
                g_norm.have_end_id ? g_norm.last_end_id : "",
                (int)g_norm.have_end_id,
                (unsigned)g_norm.n_ids
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
        if (!g_norm.have_start_gen || !g_norm.have_end_gen
            || g_norm.end_generation != g_norm.start_generation) {
            fprintf(
                stderr,
                "FAIL %s:%d: normal END episode generation does not match START "
                "(start_gen=%u end_gen=%u have_start=%d have_end=%d)\n",
                __FILE__,
                __LINE__,
                (unsigned)g_norm.start_generation,
                (unsigned)g_norm.end_generation,
                (int)g_norm.have_start_gen,
                (int)g_norm.have_end_gen
            );
            g_failures++;
        }
    }
    if (g_crit.starts != 0u || g_crit.ends != 0u) {
        if (g_crit.starts != 1u || g_crit.ends != 1u
            || !g_crit.have_start_ts || !g_crit.have_end_ts) {
            fprintf(
                stderr,
                "FAIL %s:%d: Critical START/END incomplete "
                "(starts=%u ends=%u have_start=%d have_end=%d)\n",
                __FILE__,
                __LINE__,
                (unsigned)g_crit.starts,
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
        if (g_crit.periodics != 0u) {
            if (g_crit.periodics != 1u || !g_crit.have_periodic_ts
                || !g_crit.have_periodic_oid
                || !g_crit.have_start_ts || !g_crit.have_end_ts
                || strcmp(g_crit.periodic_oid, g_crit.start_oid) != 0
                || g_crit.periodic_capture_mono_us
                       < g_crit.last_start_capture_mono_us
                || g_crit.last_end_capture_mono_us
                       < g_crit.periodic_capture_mono_us) {
                fprintf(
                    stderr,
                    "FAIL %s:%d: Critical PERIODIC not between START and END "
                    "(start=%lld periodic=%lld end=%lld periodics=%u)\n",
                    __FILE__,
                    __LINE__,
                    (long long)g_crit.last_start_capture_mono_us,
                    (long long)g_crit.periodic_capture_mono_us,
                    (long long)g_crit.last_end_capture_mono_us,
                    (unsigned)g_crit.periodics
                );
                g_failures++;
            }
        }
        if (!g_crit.have_start_oid || !g_crit.have_end_oid
            || g_crit.start_oid[0] == '\0'
            || strcmp(g_crit.end_oid, g_crit.start_oid) != 0) {
            fprintf(
                stderr,
                "FAIL %s:%d: Critical END observation_id does not match START "
                "(start='%s' end='%s')\n",
                __FILE__,
                __LINE__,
                g_crit.have_start_oid ? g_crit.start_oid : "",
                g_crit.have_end_oid ? g_crit.end_oid : ""
            );
            g_failures++;
        }
    }
}
#undef P3A_REQUIRE

/* ========== Package 4 ========== */

/**
 * AS-001 and AS-002 through the production listener and ear_sig_product_poll.
 * No planted slots, classifications, reservations, or bindings.
 * Critical is a separate listener and is polled after the product poll.
 */
#define P4_REQUIRE(cond)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
            goto p4_cleanup;                                                   \
        }                                                                      \
    } while (0)

static void p4_fill_broadband(ae_signature_t *s, uint32_t seq, float rms)
{
    memset(s, 0, sizeof(*s));
    s->schema_version = AE_SIGNATURE_SCHEMA_VERSION;
    s->window_seq = seq;
    s->window_end_mono_us = (int64_t)seq * 1000000LL;
    s->window_ms = 1000u;
    s->mic_mask = 0x1u;
    s->quality = AE_QUALITY_OK;
    s->flags = AE_FLAG_PAIR_MISSING;
    s->rms_dbfs = rms;
    s->peak_dbfs = rms + 8.0f;
    s->crest_db = 8.0f;
    s->band_dbfs[0] = rms - 2.0f;
    s->band_dbfs[1] = rms - 1.0f;
    s->band_dbfs[2] = rms - 1.5f;
    s->band_dbfs[3] = rms - 3.0f;
    s->hl_valid = true;
    s->spectral_valid = true;
    s->dominant_hz = 1800.0f;
    s->flatness = 0.70f;
    s->centroid_hz = 2200.0f;
    s->bandwidth_hz = 2500.0f;
}

static void p4_fill_heli(ae_signature_t *s, uint32_t seq, float rms)
{
    p4_fill_broadband(s, seq, rms);
    s->peak_dbfs = rms + 7.3f;
    s->crest_db = 7.3f;
    s->band_dbfs[0] = rms - 4.21f;
    s->band_dbfs[1] = rms - 3.52f;
    s->band_dbfs[2] = rms - 13.10f;
    s->band_dbfs[3] = rms - 32.77f;
    s->dominant_hz = 230.0f;
    s->flatness = 0.141f;
    s->centroid_hz = 800.0f;
    s->bandwidth_hz = 920.0f;
}

static void p4_fill_siren(ae_signature_t *s, uint32_t seq, float rms)
{
    p4_fill_broadband(s, seq, rms);
    s->peak_dbfs = rms + 7.0f;
    s->crest_db = 7.0f;
    s->band_dbfs[0] = rms - 3.0f;
    s->band_dbfs[1] = rms;
    s->band_dbfs[2] = rms - 6.0f;
    s->band_dbfs[3] = rms - 12.0f;
    s->dominant_hz = 344.0f;
    s->flatness = 0.28f;
    s->centroid_hz = 400.0f;
    s->bandwidth_hz = 80.0f;
}

static bool p4_find_as001_candidate(const ce_engine_t *ce, uint64_t *out_id)
{
    if (ce == NULL || out_id == NULL) {
        return false;
    }
    for (uint8_t i = 0u; i < CE_MAX_CANDIDATES; i++) {
        ae_candidate_t c;
        ce_classification_t cls;
        if (!ce_get_candidate(ce, i, &c)) {
            continue;
        }
        if (!ce_get_classification(ce, c.candidate_id, &cls)) {
            continue;
        }
        if (cls.valid && cls.matched && cls.profile_id == AS_PROFILE_ID_AS001) {
            *out_id = c.candidate_id;
            return true;
        }
    }
    return false;
}

static bool p4_as002_absent_ms(const ear_obs_manager_t *om, uint32_t *out_ms)
{
    if (om == NULL || out_ms == NULL) {
        return false;
    }
    bool found = false;
    for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (!om->bind[i].in_use || !om->bind[i].as002_sig_track) {
            continue;
        }
        if (found) {
            return false;
        }
        *out_ms = om->bind[i].as002_absent_ms;
        found = true;
    }
    return found;
}

static bool p4_publish(
    integ_t *f,
    ear_sig_product_t *prod,
    const ae_signature_t *sig
)
{
    if (!ae_publish_signature(&f->ae, sig)) {
        return false;
    }
    ear_sig_product_poll(prod, sig->window_end_mono_us);
    ear_critical_poll(&f->crit, sig->window_end_mono_us);
    return true;
}

static void test_p4_as001_as002_production_coexist(void)
{
    printf("test_p4_as001_as002_production_coexist\n");

    integ_t f;
    ear_promotion_policy_t pol;
    ear_sig_product_t prod;
    memset(&f, 0, sizeof(f));
    memset(&pol, 0, sizeof(pol));
    memset(&prod, 0, sizeof(prod));
    memset(&g_norm, 0, sizeof(g_norm));
    memset(&g_crit, 0, sizeof(g_crit));
    memset(g_p4_emits, 0, sizeof(g_p4_emits));
    g_p4_emit_n = 0u;
    g_p4_last_end_reason = EAR_OBS_END_EXPLICIT;
    g_p4_last_end_id[0] = '\0';
    g_p4_last_end_capture_mono_us = -1;
    g_p4_capture = true;
    g_uuid_n = 1u;

    bool se_bound = false;
    bool crit_bound = false;
    bool ce_started = false;
    bool sig_started = false;
    bool om_ready = false;
    uint64_t as001_cand = 0u;
    uint64_t as002_cand = 0u;
    char as001_obs[SP_OBS_EPISODE_ID_LEN];
    char as002_obs[SP_OBS_EPISODE_ID_LEN];
    memset(as001_obs, 0, sizeof(as001_obs));
    memset(as002_obs, 0, sizeof(as002_obs));

    ae_config_t acfg;
    ae_config_set_defaults(&acfg);
    P4_REQUIRE(ae_start(&f.ae, &acfg));
    P4_REQUIRE(sig_engine_start());
    sig_started = true;

    ce_config_t ccfg;
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 1u;
    P4_REQUIRE(ccfg.valid_miss_limit == 16u);
    P4_REQUIRE(ce_start(&f.ce, &ccfg));
    ce_started = true;

    sp_episode_registry_init(&f.reg);
    ear_obs_manager_config_t mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.registry = &f.reg;
    mcfg.ce = &f.ce;
    mcfg.ae = &f.ae;
    mcfg.uuid_fn = fake_uuid;
    mcfg.observation_interval_ms = 5000u;
    mcfg.pending_timeout_ms = 30000u;
    mcfg.detached_end_timeout_ms = 2000u;
    mcfg.as002_signature_absence_hold_ms = 3000u;
    mcfg.producer_tag = 2u;
    mcfg.emit_start = norm_emit_start;
    mcfg.emit_periodic = norm_emit_periodic;
    mcfg.emit_end = norm_emit_end;
    mcfg.enable_poll_periodic = false;
    ear_obs_manager_init(&f.om, &mcfg);
    om_ready = true;
    ce_set_promotion_cb(&f.ce, ear_obs_manager_on_promotion, &f.om);
    ear_promotion_policy_init(&pol, &f.ce, &f.om);
    ear_promotion_policy_use_sig_engine_as002(&pol, true);

    prod.om = &f.om;
    prod.policy = &pol;
    prod.ce = &f.ce;
    prod.ready = true;
    /* Production order: Signature Engine product listener before CE. */
    P4_REQUIRE(ae_add_signature_listener(
        &f.ae, ear_sig_product_on_ae_signature, &prod
    ));
    se_bound = true;
    P4_REQUIRE(ce_bind_audio_engine(&f.ce, &f.ae));
    P4_REQUIRE(ae_listener_count(&f.ae) == 2u);

    ear_critical_config_t ccrit;
    ear_critical_config_set_defaults(&ccrit);
    ccrit.auto_detect_enabled = false;
    ccrit.lab_impulse_enabled = false;
    ccrit.publish = crit_publish;
    ccrit.uuid_fn = crit_uuid;
    ccrit.periodic_ms = 5000u;
    ccrit.idle_timeout_ms = 15000u;
    ccrit.max_active_ms = 30000u;
    P4_REQUIRE(ear_critical_init(&f.crit, &ccrit));
    P4_REQUIRE(ae_add_signature_listener(&f.ae, critical_ae_listener, &f.crit));
    crit_bound = true;
    P4_REQUIRE(ae_listener_count(&f.ae) == 3u);

    ae_signature_t sig;
    for (uint32_t seq = 1u; seq <= 4u; seq++) {
        p4_fill_broadband(&sig, seq, -55.0f);
        P4_REQUIRE(p4_publish(&f, &prod, &sig));
    }
    P4_REQUIRE(g_norm.starts == 0u);
    P4_REQUIRE(!ear_critical_is_active(&f.crit));

    /* The same shape at background level fails AS-001 RMS eligibility. */
    p4_fill_heli(&sig, 5u, -55.0f);
    P4_REQUIRE(p4_publish(&f, &prod, &sig));
    sig_primary_distance_t primary;
    P4_REQUIRE(!sig_engine_get_primary_distance_match(&primary) || !primary.valid);
    P4_REQUIRE(g_norm.starts == 0u);

    p4_fill_heli(&sig, 6u, -40.0f);
    P4_REQUIRE(p4_publish(&f, &prod, &sig));
    P4_REQUIRE(sig_engine_get_primary_distance_match(&primary) && primary.valid);
    P4_REQUIRE(primary.profile_id == AS_PROFILE_ID_AS001);
    P4_REQUIRE(primary.window_seq == sig.window_seq);
    P4_REQUIRE(!sig_engine_allows_promote(AS_PROFILE_ID_AS002));
    P4_REQUIRE(g_norm.starts == 1u);
    P4_REQUIRE(ear_obs_manager_n_open(&f.om) == 1u);
    P4_REQUIRE(p4_find_as001_candidate(&f.ce, &as001_cand));
    P4_REQUIRE(as001_cand != 0u);
    {
        const int bi = find_open_bind_idx(&f.om, as001_cand);
        P4_REQUIRE(bi >= 0);
        snprintf(
            as001_obs,
            sizeof(as001_obs),
            "%s",
            f.om.bind[bi].observation_id
        );
    }
    P4_REQUIRE(p4_emit_path_is(as001_obs, true, "AIRCRAFT"));
    {
        ear_cls_assessment_wire_t stored;
        P4_REQUIRE(ear_obs_manager_get_cls_assessment(&f.om, as001_cand, &stored));
        P4_REQUIRE(p4_path_is(&stored, "AIRCRAFT"));
    }

    /* Two tonal windows stay under the three-window ALARM gate. */
    for (uint32_t seq = 7u; seq <= 8u; seq++) {
        p4_fill_siren(&sig, seq, -31.0f);
        P4_REQUIRE(p4_publish(&f, &prod, &sig));
        P4_REQUIRE(g_norm.starts == 1u);
        P4_REQUIRE(ear_obs_manager_n_open(&f.om) == 1u);
        P4_REQUIRE(sig_engine_as002_consecutive() == (seq - 6u));
        P4_REQUIRE(!sig_engine_allows_promote(AS_PROFILE_ID_AS002));
    }

    p4_fill_siren(&sig, 9u, -31.0f);
    P4_REQUIRE(p4_publish(&f, &prod, &sig));
    P4_REQUIRE(sig_engine_allows_promote(AS_PROFILE_ID_AS002));
    P4_REQUIRE(!sig_engine_get_primary_distance_match(&primary) || !primary.valid);
    P4_REQUIRE(g_norm.starts == 2u);
    P4_REQUIRE(ear_obs_manager_n_open(&f.om) == 2u);
    P4_REQUIRE(strcmp(g_norm.ids[0], g_norm.ids[1]) != 0);
    snprintf(as002_obs, sizeof(as002_obs), "%s", g_norm.ids[1]);
    P4_REQUIRE(strcmp(as001_obs, as002_obs) != 0);
    P4_REQUIRE(p4_emit_path_is(as002_obs, true, "ALARM"));
    {
        bool found_other = false;
        for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
            if (!f.om.bind[i].in_use) {
                continue;
            }
            if (f.om.bind[i].candidate_id == as001_cand) {
                continue;
            }
            as002_cand = f.om.bind[i].candidate_id;
            found_other = true;
        }
        P4_REQUIRE(found_other && as002_cand != 0u && as002_cand != as001_cand);
        ae_candidate_t siren;
        P4_REQUIRE(ce_find_candidate(&f.ce, as002_cand, &siren));
        P4_REQUIRE(siren.last_window_seq == sig.window_seq);
        P4_REQUIRE(siren.evidence_count == EAR_SIREN_ALARM_MIN_STREAK);
    }
    /*
     * Stored AS-001 assessment, read before any AS-001 PERIODIC/END emit
     * can deepen or rewrite the wire.
     */
    {
        ear_cls_assessment_wire_t stored;
        P4_REQUIRE(ear_obs_manager_get_cls_assessment(&f.om, as001_cand, &stored));
        P4_REQUIRE(p4_path_is(&stored, "AIRCRAFT"));
    }

    p4_fill_siren(&sig, 10u, -31.0f);
    P4_REQUIRE(p4_publish(&f, &prod, &sig));
    p4_fill_siren(&sig, 11u, -31.0f);
    P4_REQUIRE(p4_publish(&f, &prod, &sig));
    P4_REQUIRE(sig_engine_as002_consecutive() == EAR_SIREN_SE_MIN_STREAK);
    {
        ear_cls_assessment_wire_t as001_stored;
        ear_cls_assessment_wire_t as002_stored;
        P4_REQUIRE(ear_obs_manager_get_cls_assessment(
            &f.om, as001_cand, &as001_stored
        ));
        P4_REQUIRE(p4_path_is(&as001_stored, "AIRCRAFT"));
        P4_REQUIRE(ear_obs_manager_get_cls_assessment(
            &f.om, as002_cand, &as002_stored
        ));
        P4_REQUIRE(p4_path_is(&as002_stored, "ALARM/SE_PUBLIC_ALERT"));
    }

    ear_obs_manager_on_activity_periodic_hint(&f.om, 5000000, 10000000, true, -31.0f);
    P4_REQUIRE(p4_emit_path_is(as001_obs, false, "AIRCRAFT/Helicopter"));
    P4_REQUIRE(p4_emit_path_is(as002_obs, false, "ALARM/SE_PUBLIC_ALERT"));
    P4_REQUIRE(g_norm.periodics == 2u);
    P4_REQUIRE(g_norm.ends == 0u);

    /* Absence is advanced only by the product listener's window forward. */
    {
        ear_obs_manager_diag_t d0;
        ear_obs_manager_get_diag(&f.om, &d0);
        const uint32_t adv0 = d0.as002_sig_absent_advance;
        const uint32_t pause0 = d0.as002_sig_absent_pause;
        const uint32_t reset0 = d0.as002_sig_support_reset;
        const uint32_t ends0 = g_norm.ends;

        p4_fill_broadband(&sig, 12u, -55.0f);
        P4_REQUIRE(p4_publish(&f, &prod, &sig));
        ear_obs_manager_diag_t d;
        ear_obs_manager_get_diag(&f.om, &d);
        P4_REQUIRE(d.as002_sig_absent_advance == adv0 + 1u);
        P4_REQUIRE(g_norm.ends == ends0);
        P4_REQUIRE(ear_obs_manager_n_open(&f.om) == 2u);
        uint32_t absent_ms = 0u;
        P4_REQUIRE(p4_as002_absent_ms(&f.om, &absent_ms));
        P4_REQUIRE(absent_ms != 0u);
        const uint32_t accumulated_ms = absent_ms;

        p4_fill_broadband(&sig, 13u, -55.0f);
        sig.quality = AE_QUALITY_GAP;
        P4_REQUIRE(p4_publish(&f, &prod, &sig));
        ear_obs_manager_get_diag(&f.om, &d);
        P4_REQUIRE(d.as002_sig_absent_pause == pause0 + 1u);
        P4_REQUIRE(d.as002_sig_absent_advance == adv0 + 1u);
        P4_REQUIRE(p4_as002_absent_ms(&f.om, &absent_ms));
        P4_REQUIRE(absent_ms == accumulated_ms);
        P4_REQUIRE(g_norm.ends == ends0);

        p4_fill_broadband(&sig, 14u, -55.0f);
        sig.quality = AE_QUALITY_INVALID;
        P4_REQUIRE(p4_publish(&f, &prod, &sig));
        ear_obs_manager_get_diag(&f.om, &d);
        P4_REQUIRE(d.as002_sig_absent_pause == pause0 + 2u);
        P4_REQUIRE(d.as002_sig_absent_advance == adv0 + 1u);
        P4_REQUIRE(p4_as002_absent_ms(&f.om, &absent_ms));
        P4_REQUIRE(absent_ms == accumulated_ms);
        P4_REQUIRE(g_norm.ends == ends0);

        p4_fill_siren(&sig, 15u, -31.0f);
        P4_REQUIRE(p4_publish(&f, &prod, &sig));
        ear_obs_manager_get_diag(&f.om, &d);
        P4_REQUIRE(d.as002_sig_support_reset == reset0 + 1u);
        P4_REQUIRE(p4_as002_absent_ms(&f.om, &absent_ms));
        P4_REQUIRE(absent_ms == 0u);
        P4_REQUIRE(g_norm.ends == ends0);
        P4_REQUIRE(ear_obs_manager_n_open(&f.om) == 2u);

        p4_fill_broadband(&sig, 16u, -55.0f);
        P4_REQUIRE(p4_publish(&f, &prod, &sig));
        P4_REQUIRE(p4_as002_absent_ms(&f.om, &absent_ms));
        P4_REQUIRE(absent_ms == accumulated_ms);
        p4_fill_broadband(&sig, 17u, -55.0f);
        P4_REQUIRE(p4_publish(&f, &prod, &sig));
        P4_REQUIRE(ear_obs_manager_n_open(&f.om) == 2u);
        P4_REQUIRE(g_norm.ends == ends0);

        p4_fill_broadband(&sig, 18u, -55.0f);
        const int64_t cross_capture_mono_us = sig.window_end_mono_us;
        P4_REQUIRE(p4_publish(&f, &prod, &sig));
        P4_REQUIRE(g_norm.ends == ends0 + 1u);
        P4_REQUIRE(g_p4_last_end_reason == EAR_OBS_END_SIGNATURE);
        P4_REQUIRE(strcmp(g_p4_last_end_id, as002_obs) == 0);
        P4_REQUIRE(g_p4_last_end_capture_mono_us == cross_capture_mono_us);
        P4_REQUIRE(ear_obs_manager_n_open(&f.om) == 1u);
        P4_REQUIRE(ear_obs_manager_has_signature_observation(
            &f.om, AS_PROFILE_ID_AS001
        ));
        P4_REQUIRE(!ear_obs_manager_has_signature_observation(
            &f.om, AS_PROFILE_ID_AS002
        ));
        P4_REQUIRE(find_open_bind_idx(&f.om, as001_cand) >= 0);
    }

    /* Recover the promoted helicopter track before the valid-miss budget. */
    ae_candidate_t before_rematch;
    P4_REQUIRE(ce_find_candidate(&f.ce, as001_cand, &before_rematch));
    P4_REQUIRE(before_rematch.state == AE_CAND_COASTING);
    p4_fill_heli(&sig, 19u, -40.0f);
    P4_REQUIRE(p4_publish(&f, &prod, &sig));
    ae_candidate_t helicopter;
    P4_REQUIRE(ce_find_candidate(&f.ce, as001_cand, &helicopter));
    P4_REQUIRE(helicopter.state == AE_CAND_PROMOTED);
    P4_REQUIRE(helicopter.promotion_gen == before_rematch.promotion_gen);
    P4_REQUIRE(helicopter.valid_miss_windows == 0u);
    P4_REQUIRE(g_norm.starts == 2u);

    for (uint32_t seq = 20u; seq < 36u; seq++) {
        p4_fill_broadband(&sig, seq, -55.0f);
        P4_REQUIRE(p4_publish(&f, &prod, &sig));
        P4_REQUIRE(g_norm.ends == 1u);
    }
    P4_REQUIRE(!ce_find_candidate(&f.ce, as001_cand, &before_rematch));
    const int detached = find_open_bind_idx(&f.om, as001_cand);
    P4_REQUIRE(detached >= 0 && !f.om.bind[detached].ce_live);

    ear_obs_manager_on_promotion(&helicopter, CE_PROMOTION_ENTER, &f.om);
    ear_sig_product_poll(&prod, 36000000);
    P4_REQUIRE(!f.om.bind[detached].ce_live);
    P4_REQUIRE(g_norm.ends == 1u);
    ear_sig_product_poll(&prod, 37000000);
    P4_REQUIRE(g_norm.ends == 2u);
    P4_REQUIRE(strcmp(g_p4_last_end_id, as001_obs) == 0);
    ear_obs_manager_on_promotion(&helicopter, CE_PROMOTION_ENTER, &f.om);
    ear_obs_manager_on_promotion(&helicopter, CE_PROMOTION_EXIT, &f.om);
    ear_obs_manager_on_candidate_evidence(&f.om, as001_cand, NULL, &helicopter);
    ear_sig_product_poll(&prod, 38000000);
    P4_REQUIRE(g_norm.starts == 2u && g_norm.ends == 2u);
    P4_REQUIRE(ear_obs_manager_n_open(&f.om) == 0u);
    P4_REQUIRE(ear_obs_manager_n_held(&f.om) == 0u);
    P4_REQUIRE(ear_obs_manager_n_pending(&f.om) == 0u);
    P4_REQUIRE(sp_episode_registry_in_use_count(&f.reg) == 0u);

    /* Siren alone can admit again after both earlier Observations have ended. */
    for (uint32_t seq = 39u; seq <= 43u; seq++) {
        p4_fill_siren(&sig, seq, -31.0f);
        P4_REQUIRE(p4_publish(&f, &prod, &sig));
        P4_REQUIRE(!sig_engine_get_primary_distance_match(&primary) || !primary.valid);
        P4_REQUIRE(g_norm.starts == (seq < 41u ? 2u : 3u));
    }
    P4_REQUIRE(ear_obs_manager_n_open(&f.om) == 1u);
    P4_REQUIRE(ear_obs_manager_has_signature_observation(&f.om, AS_PROFILE_ID_AS002));
    const char *new_siren_id = NULL;
    for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (f.om.bind[i].in_use) {
            new_siren_id = f.om.bind[i].observation_id;
        }
    }
    P4_REQUIRE(new_siren_id != NULL);
    P4_REQUIRE(strcmp(new_siren_id, as002_obs) != 0);
    P4_REQUIRE(p4_emit_path_is(new_siren_id, true, "ALARM"));

    P4_REQUIRE(!ear_critical_is_active(&f.crit));
    P4_REQUIRE(g_crit.starts == 0u);
    {
        ear_critical_diag_t cd;
        ear_critical_get_diag(&f.crit, &cd);
        P4_REQUIRE(cd.auto_start_blocked >= 1u);
        P4_REQUIRE(cd.start == 0u);
    }

p4_cleanup:
    g_p4_capture = false;
    if (om_ready && (ear_obs_manager_n_open(&f.om) > 0u
                     || ear_obs_manager_n_pending(&f.om) > 0u)) {
        ear_obs_manager_force_end_all(&f.om, EAR_OBS_END_EXPLICIT, 20000000);
        ear_obs_manager_poll(&f.om, 21000000);
    }
    if (ear_critical_is_active(&f.crit)) {
        ear_critical_force_end(&f.crit, 22000000, EAR_CRITICAL_END_EXPLICIT);
    }
    if (crit_bound) {
        (void)ae_remove_signature_listener(&f.ae, critical_ae_listener, &f.crit);
    }
    if (ce_started) {
        ce_set_promotion_cb(&f.ce, NULL, NULL);
        ce_stop(&f.ce);
        if (om_ready) {
            ear_obs_manager_poll(&f.om, 23000000);
        }
    }
    if (se_bound) {
        (void)ae_remove_signature_listener(
            &f.ae, ear_sig_product_on_ae_signature, &prod
        );
    }
    if (sig_started) {
        sig_engine_stop();
    }
}
#undef P4_REQUIRE

static int run_existing_host_tests(void)
{
    test4_critical_with_both_normal_slots_occupied();
    test5_ota_cleanup_while_critical_active();
    test_p3a_ae_listener_triad_critical_coexist();
    test_p4_as001_as002_production_coexist();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}

#ifdef DBMAP_OBS_PUBLISH_CONTRACT_EXPORT

#define CONTRACT_REPORTS 7
#define CONTRACT_CAP 8

typedef struct {
    char observation_id[64];
    char kind[16];
    char origin[32];
    char termination[32];
    int64_t interval_start_us;
    int64_t capture_us;
    double interval_ms;
    double episode_ms;
    bool has_level;
    double level_dbfs;
    bool has_assessment;
    ear_cls_assessment_wire_t assessment;
} contract_rec_t;

static bool g_contract_capture;
static bool g_contract_overflow;
static uint32_t g_contract_n;
static contract_rec_t g_contract[CONTRACT_CAP];

static void contract_copy(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_len, "%s", src);
}

static void contract_push(const contract_rec_t *rec)
{
    if (!g_contract_capture || rec == NULL) {
        return;
    }
    if (g_contract_n >= CONTRACT_CAP) {
        g_contract_overflow = true;
        return;
    }
    g_contract[g_contract_n++] = *rec;
}

static void contract_note_normal(
    const char *kind,
    const ear_obs_binding_view_t *binding,
    const ear_obs_promote_snapshot_t *snap,
    int64_t interval_start_mono_us,
    int64_t capture_mono_us,
    bool has_level,
    float level_dbfs
)
{
    contract_rec_t rec;
    double level = (double)level_dbfs;
    if (!g_contract_capture || binding == NULL || kind == NULL) {
        return;
    }
    memset(&rec, 0, sizeof(rec));
    contract_copy(rec.observation_id, sizeof(rec.observation_id), binding->observation_id);
    contract_copy(rec.kind, sizeof(rec.kind), kind);
    rec.interval_start_us = interval_start_mono_us;
    rec.capture_us = capture_mono_us;
    if (capture_mono_us > interval_start_mono_us) {
        rec.interval_ms = (double)(capture_mono_us - interval_start_mono_us) / 1000.0;
    }
    if (capture_mono_us > binding->start_mono_us) {
        rec.episode_ms = (double)(capture_mono_us - binding->start_mono_us) / 1000.0;
    }
    if (snap != NULL) {
        rec.has_level = true;
        level = (double)snap->ema_rms_dbfs;
    } else {
        rec.has_level = has_level;
    }
    rec.level_dbfs = level;
    if (binding->assessment.present && binding->assessment.n_nodes > 0u) {
        rec.has_assessment = true;
        rec.assessment = binding->assessment;
    }
    contract_push(&rec);
}

static void contract_note_critical(
    const char *observation_id,
    const char *emission_kind,
    const char *termination_reason,
    const char *origin,
    int64_t interval_start_mono_us,
    int64_t capture_mono_us,
    double interval_duration_ms,
    double episode_duration_ms
)
{
    contract_rec_t rec;
    if (!g_contract_capture || observation_id == NULL || emission_kind == NULL) {
        return;
    }
    memset(&rec, 0, sizeof(rec));
    contract_copy(rec.observation_id, sizeof(rec.observation_id), observation_id);
    contract_copy(rec.kind, sizeof(rec.kind), emission_kind);
    contract_copy(rec.origin, sizeof(rec.origin), origin);
    contract_copy(rec.termination, sizeof(rec.termination), termination_reason);
    rec.interval_start_us = interval_start_mono_us;
    rec.capture_us = capture_mono_us;
    rec.interval_ms = interval_duration_ms;
    rec.episode_ms = episode_duration_ms;
    contract_push(&rec);
}

static void format_fixed_utc(int64_t mono_us, char *out, size_t out_len)
{
    int64_t usec;
    int64_t total_sec;
    int64_t sec;
    int64_t min;
    int64_t hour;
    if (mono_us < 0) {
        mono_us = 0;
    }
    usec = mono_us % 1000000LL;
    total_sec = mono_us / 1000000LL;
    sec = total_sec % 60LL;
    total_sec /= 60LL;
    min = total_sec % 60LL;
    hour = total_sec / 60LL;
    snprintf(
        out,
        out_len,
        "2026-01-01T%02lld:%02lld:%02lld.%06lldZ",
        (long long)hour,
        (long long)min,
        (long long)sec,
        (long long)usec
    );
}

static void fill_empty_l0(dbmap_l0_interval_t *iv)
{
    memset(iv, 0, sizeof(*iv));
    iv->sample_rate_hz = 16000u;
    iv->analysis_window_ms = 1000u;
}

static void fill_scene_l0(dbmap_l0_interval_t *iv)
{
    fill_empty_l0(iv);
    iv->window_count = 4u;
    iv->clipped_window_count = 1u;
    iv->n_crest = 3u;
    iv->has_rms_stats = true;
    iv->rms_mean_dbfs = -31.5f;
    iv->rms_min_dbfs = -40.0f;
    iv->rms_max_dbfs = -28.0f;
    iv->has_band_means = true;
    iv->band_mean_dbfs[0] = -33.0f;
    iv->band_mean_dbfs[1] = -32.0f;
    iv->band_mean_dbfs[2] = -36.0f;
    iv->band_mean_dbfs[3] = -42.0f;
    iv->has_band_fractions = true;
    iv->band_fraction[0] = 0.5f;
    iv->band_fraction[1] = 0.25f;
    iv->band_fraction[2] = 0.125f;
    iv->band_fraction[3] = 0.125f;
    iv->has_hl_mean = true;
    iv->hl_mean_db = -3.5f;
    iv->has_t1 = true;
    iv->rms_stdev_db = 1.5f;
    iv->rms_range_db = 12.0f;
    iv->has_t2 = true;
    iv->rms_slope_db_per_s = 0.25f;
    iv->has_t3 = true;
    iv->crest_max_db = 11.0f;
    iv->high_crest_count = 2u;
    iv->high_crest_rate = 0.5f;
    iv->has_t6 = true;
    iv->hl_slope_db_per_s = -0.125f;
}

static void fill_scene_l1(dbmap_l1_interval_t *iv)
{
    memset(iv, 0, sizeof(*iv));
    iv->sample_rate_hz = 16000u;
    iv->analysis_window_ms = 1000u;
    iv->fft_size = DBMAP_L1_FFT_SIZE;
    iv->freq_min_hz = DBMAP_L1_FREQ_MIN_HZ;
    iv->freq_max_hz = DBMAP_L1_FREQ_MAX_HZ;
    iv->window_count = 4u;
    iv->invalid_window_count = 1u;
    iv->has_centroid = true;
    iv->centroid_mean_hz = 812.5f;
    iv->centroid_stdev_hz = 16.0f;
    iv->has_bandwidth = true;
    iv->bandwidth_mean_hz = 920.0f;
    iv->bandwidth_stdev_hz = 8.0f;
    iv->has_dominant = true;
    iv->dominant_mean_hz = 230.0f;
    iv->dominant_stdev_hz = 4.0f;
    iv->has_flatness = true;
    iv->flatness_mean = 0.25f;
    iv->flatness_stdev = 0.0625f;
    iv->has_stdev = true;
}

static void fill_scene_l2(dbmap_l2_interval_t *iv)
{
    memset(iv, 0, sizeof(*iv));
    iv->sample_rate_hz = 16000u;
    iv->analysis_window_ms = 1000u;
    iv->fft_size = DBMAP_L1_FFT_SIZE;
    iv->freq_min_hz = DBMAP_L1_FREQ_MIN_HZ;
    iv->freq_max_hz = DBMAP_L1_FREQ_MAX_HZ;
    iv->frame_hop_samples = DBMAP_L1_FFT_SIZE;
    iv->window_count = 4u;
    iv->invalid_window_count = 0u;
    iv->history_overflow = false;
    iv->spectral_flux_valid_count = 4u;
    iv->has_spectral_flux_mean = true;
    iv->spectral_flux_mean = 0.375f;
    iv->has_spectral_flux_stdev = true;
    iv->spectral_flux_stdev = 0.125f;
    iv->envelope_cv_valid_count = 4u;
    iv->has_envelope_cv_mean = true;
    iv->envelope_cv_mean = 0.25f;
    iv->has_envelope_cv_stdev = true;
    iv->envelope_cv_stdev = 0.0625f;
    iv->dominant_delta_valid_count = 3u;
    iv->has_dominant_delta_mean = true;
    iv->dominant_delta_mean_hz = 15.625f;
    iv->has_dominant_delta_stdev = true;
    iv->dominant_delta_stdev_hz = 0.5f;
}

static bool contract_write_reports(const char *dir, const char *node_id, const char *topic)
{
    dbmap_l0_interval_t empty_l0;
    dbmap_l0_interval_t scene_l0;
    dbmap_l1_interval_t scene_l1;
    dbmap_l2_interval_t scene_l2;
    char topic_path[512];
    FILE *topic_fp;
    uint32_t i;

    fill_empty_l0(&empty_l0);
    fill_scene_l0(&scene_l0);
    fill_scene_l1(&scene_l1);
    fill_scene_l2(&scene_l2);

    snprintf(topic_path, sizeof(topic_path), "%s/topic.txt", dir);
    topic_fp = fopen(topic_path, "w");
    if (topic_fp == NULL) {
        fprintf(stderr, "contract export: cannot write %s\n", topic_path);
        return false;
    }
    if (fprintf(topic_fp, "%s\n", topic) < 0) {
        fclose(topic_fp);
        return false;
    }
    fclose(topic_fp);

    for (i = 0u; i < g_contract_n; i++) {
        const contract_rec_t *rec = &g_contract[i];
        const bool critical = rec->origin[0] != '\0';
        const bool scene = !critical && strcmp(rec->kind, "periodic") == 0;
        char event_time[40];
        char path[512];
        dbmap_observation_fields_t fields;
        char *json;
        FILE *fp;
        memset(&fields, 0, sizeof(fields));
        format_fixed_utc(rec->interval_start_us, event_time, sizeof(event_time));
        fields.observation_id = rec->observation_id;
        fields.node_id = node_id;
        fields.hub_id = "";
        fields.sequence_number = i + 1u;
        fields.event_time_utc = event_time;
        fields.capture_timestamp_monotonic_us = rec->capture_us;
        fields.timing_quality = "estimated";
        fields.timestamp_uncertainty_ms = 10.0;
        fields.interval_duration_ms = rec->interval_ms;
        fields.episode_duration_ms = rec->episode_ms;
        fields.emission_kind = rec->kind;
        fields.termination_reason = rec->termination[0] != '\0' ? rec->termination : NULL;
        fields.origin = critical ? rec->origin : NULL;
        fields.has_signal_level_dbfs = rec->has_level && rec->level_dbfs <= 0.0;
        fields.signal_level_dbfs = rec->level_dbfs;
        fields.measurements = scene ? &scene_l0 : &empty_l0;
        fields.signature = scene ? &scene_l1 : NULL;
        fields.dynamics = scene ? &scene_l2 : NULL;
        fields.assessment =
            (!critical && rec->has_assessment) ? &rec->assessment : NULL;
        json = dbmap_observation_build_json(&fields);
        if (json == NULL) {
            fprintf(
                stderr,
                "contract export: JSON build failed for report %u kind=%s\n",
                i + 1u,
                rec->kind
            );
            return false;
        }
        snprintf(path, sizeof(path), "%s/report-%02u.json", dir, i + 1u);
        fp = fopen(path, "wb");
        if (fp == NULL) {
            fprintf(stderr, "contract export: cannot write %s\n", path);
            free(json);
            return false;
        }
        if (fputs(json, fp) == EOF) {
            fclose(fp);
            free(json);
            return false;
        }
        fclose(fp);
        free(json);
    }
    return true;
}

static int contract_export_main(const char *dir)
{
    integ_t f;
    ear_promotion_policy_t pol;
    ear_sig_product_t prod;
    ae_signature_t sig;
    sp_effective_t topics;
    bool se_bound = false;
    bool crit_bound = false;
    bool ce_started = false;
    bool sig_started = false;
    bool om_ready = false;
    int rc = 1;
    const char *node_id = "EAR-CONTRACT";
    const int64_t crit_start_us = 12000000;
    const int64_t crit_periodic_us = 17000000;
    const int64_t crit_end_us = 18000000;

    if (dir == NULL || dir[0] == '\0') {
        fprintf(stderr, "contract export: missing directory\n");
        return 1;
    }

    memset(&f, 0, sizeof(f));
    memset(&pol, 0, sizeof(pol));
    memset(&prod, 0, sizeof(prod));
    memset(&g_norm, 0, sizeof(g_norm));
    memset(&g_crit, 0, sizeof(g_crit));
    memset(g_contract, 0, sizeof(g_contract));
    g_contract_n = 0u;
    g_contract_overflow = false;
    g_contract_capture = false;
    g_p4_capture = false;
    g_uuid_n = 1u;

    {
        ae_config_t acfg;
        ce_config_t ccfg;
        ear_obs_manager_config_t mcfg;
        ear_critical_config_t ccrit;
        ae_config_set_defaults(&acfg);
        if (!ae_start(&f.ae, &acfg) || !sig_engine_start()) {
            fprintf(stderr, "contract export: engine start failed\n");
            goto export_cleanup;
        }
        sig_started = true;
        ce_config_set_defaults(&ccfg);
        ccfg.n_enter = 1u;
        ccfg.t_coast = 64u;
        if (!ce_start(&f.ce, &ccfg)) {
            fprintf(stderr, "contract export: candidate engine start failed\n");
            goto export_cleanup;
        }
        ce_started = true;
        sp_episode_registry_init(&f.reg);
        memset(&mcfg, 0, sizeof(mcfg));
        mcfg.registry = &f.reg;
        mcfg.ce = &f.ce;
        mcfg.ae = &f.ae;
        mcfg.uuid_fn = fake_uuid;
        mcfg.observation_interval_ms = 5000u;
        mcfg.pending_timeout_ms = 30000u;
        mcfg.detached_end_timeout_ms = 60000u;
        mcfg.as002_signature_absence_hold_ms = 3000u;
        mcfg.producer_tag = 2u;
        mcfg.emit_start = norm_emit_start;
        mcfg.emit_periodic = norm_emit_periodic;
        mcfg.emit_end = norm_emit_end;
        mcfg.enable_poll_periodic = false;
        ear_obs_manager_init(&f.om, &mcfg);
        om_ready = true;
        ce_set_promotion_cb(&f.ce, ear_obs_manager_on_promotion, &f.om);
        ear_promotion_policy_init(&pol, &f.ce, &f.om);
        ear_promotion_policy_use_sig_engine_as002(&pol, true);
        prod.om = &f.om;
        prod.policy = &pol;
        prod.ce = &f.ce;
        prod.ready = true;
        if (!ae_add_signature_listener(&f.ae, ear_sig_product_on_ae_signature, &prod)) {
            fprintf(stderr, "contract export: product listener failed\n");
            goto export_cleanup;
        }
        se_bound = true;
        if (!ce_bind_audio_engine(&f.ce, &f.ae)) {
            fprintf(stderr, "contract export: CE bind failed\n");
            goto export_cleanup;
        }
        ear_critical_config_set_defaults(&ccrit);
        ccrit.auto_detect_enabled = false;
        ccrit.lab_impulse_enabled = false;
        ccrit.publish = crit_publish;
        ccrit.uuid_fn = crit_uuid;
        ccrit.periodic_ms = 5000u;
        ccrit.idle_timeout_ms = 15000u;
        ccrit.max_active_ms = 30000u;
        if (!ear_critical_init(&f.crit, &ccrit)
            || !ae_add_signature_listener(&f.ae, critical_ae_listener, &f.crit)) {
            fprintf(stderr, "contract export: critical listener failed\n");
            goto export_cleanup;
        }
        crit_bound = true;
    }

    g_contract_capture = true;
    for (uint32_t seq = 1u; seq <= 4u; seq++) {
        p4_fill_broadband(&sig, seq, -55.0f);
        if (!p4_publish(&f, &prod, &sig)) {
            fprintf(stderr, "contract export: broadband publish failed\n");
            goto export_cleanup;
        }
    }
    p4_fill_heli(&sig, 5u, -40.0f);
    if (!p4_publish(&f, &prod, &sig)) {
        fprintf(stderr, "contract export: helicopter publish failed\n");
        goto export_cleanup;
    }
    for (uint32_t seq = 6u; seq <= 10u; seq++) {
        p4_fill_siren(&sig, seq, -31.0f);
        if (!p4_publish(&f, &prod, &sig)) {
            fprintf(stderr, "contract export: siren publish failed\n");
            goto export_cleanup;
        }
    }
    ear_obs_manager_on_activity_periodic_hint(&f.om, 5000000, 10000000, true, -31.0f);

    sig = make_impulse_sig(12u);
    if (!ear_critical_force_start(&f.crit, &sig, crit_start_us)) {
        fprintf(stderr, "contract export: critical start failed\n");
        goto export_cleanup;
    }
    ear_critical_poll(&f.crit, crit_periodic_us);
    ear_critical_force_end(&f.crit, crit_end_us, EAR_CRITICAL_END_EXPLICIT);
    g_contract_capture = false;

    if (g_contract_overflow || g_contract_n != CONTRACT_REPORTS) {
        fprintf(
            stderr,
            "contract export: expected %d reports, captured %u overflow=%d\n",
            CONTRACT_REPORTS,
            g_contract_n,
            g_contract_overflow ? 1 : 0
        );
        goto export_cleanup;
    }

    memset(&topics, 0, sizeof(topics));
    snprintf(topics.identity.node_id, sizeof(topics.identity.node_id), "%s", node_id);
    sp_rebuild_topics(&topics);
    if (strcmp(sp_mqtt_topic_kind(), "esp-ear") != 0 || topics.topics.observation[0] == '\0') {
        fprintf(stderr, "contract export: topic builder did not produce an Ear observation topic\n");
        goto export_cleanup;
    }
    if (!contract_write_reports(dir, topics.identity.node_id, topics.topics.observation)) {
        goto export_cleanup;
    }
    printf("contract export: %u reports -> %s\n", g_contract_n, dir);
    rc = 0;

export_cleanup:
    g_contract_capture = false;
    if (om_ready && (ear_obs_manager_n_open(&f.om) > 0u || ear_obs_manager_n_pending(&f.om) > 0u)) {
        ear_obs_manager_force_end_all(&f.om, EAR_OBS_END_EXPLICIT, 20000000);
        ear_obs_manager_poll(&f.om, 21000000);
    }
    if (ear_critical_is_active(&f.crit)) {
        ear_critical_force_end(&f.crit, 22000000, EAR_CRITICAL_END_EXPLICIT);
    }
    if (crit_bound) {
        (void)ae_remove_signature_listener(&f.ae, critical_ae_listener, &f.crit);
    }
    if (ce_started) {
        ce_set_promotion_cb(&f.ce, NULL, NULL);
        ce_stop(&f.ce);
        if (om_ready) {
            ear_obs_manager_poll(&f.om, 23000000);
        }
    }
    if (se_bound) {
        (void)ae_remove_signature_listener(&f.ae, ear_sig_product_on_ae_signature, &prod);
    }
    if (sig_started) {
        sig_engine_stop();
    }
    return rc;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--export-publish-contract") == 0) {
        return contract_export_main(argv[2]);
    }
    return run_existing_host_tests();
}

#else

int main(void)
{
    return run_existing_host_tests();
}

#endif
