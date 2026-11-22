/**
 * Host tests: EAR Observation Manager promotion-gated contract (v1 + P1 END).
 *
 * From repo root (see also firmware/scripts/run_firmware_fast_gate.sh ae_inc):
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/candidate_engine/include \
 *     -I firmware/products/dbmap/ear/acoustic_profile/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     -I firmware/products/dbmap/ear/ear_observation/include \
 *     -I firmware/platform/observation/include \
 *     -I firmware/platform/audio/include \
 *     … ae/ce/ap sources … episode_registry.c \
 *     ear_obs_manager.c ear_obs_start_retain.c ear_promotion_policy.c \
 *     test_ear_obs_manager.c \
 *     -lm -o /tmp/test_ear_obs_manager && /tmp/test_ear_obs_manager
 */
#include "ae_engine.h"
#include "ae_signature.h"
#include "as_profile.h"
#include "ce_engine.h"
#include "ear_cls_as001.h"
#include "ear_obs_lifecycle_section.h"
#include "ear_obs_manager.h"
#include "ear_obs_start_retain.h"
#include "ear_promotion_policy.h"
#include "ear_sig_product.h"
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

typedef struct {
    uint32_t starts;
    uint32_t periodics;
    uint32_t ends;
    uint64_t last_start_cand;
    uint64_t last_periodic_cand;
    uint64_t last_end_cand;
    char last_obs_id[SP_OBS_EPISODE_ID_LEN];
    char last_end_obs_id[SP_OBS_EPISODE_ID_LEN];
    char end_obs_ids[8][SP_OBS_EPISODE_ID_LEN];
    uint32_t end_obs_n;
    int64_t last_periodic_start;
    int64_t last_periodic_end;
    bool last_periodic_has_lvl;
    float last_periodic_lvl;
    int64_t last_end_start;
    int64_t last_end_end;
    int64_t last_end_binding_start_mono_us;
    bool last_end_has_lvl;
    float last_end_lvl;
    ear_obs_end_reason_t last_end_reason;
    /** Joined assessment.path ids at last START / PERIODIC. */
    char last_start_assessment_path[96];
    bool last_start_assessment_present;
    char last_periodic_assessment_path[96];
    bool last_periodic_assessment_present;
    /** When true, emit_end returns false (publication failure seam). */
    bool fail_end;
    /** Remaining START attempts that return false, then succeed. */
    uint32_t fail_start_remaining;
    struct {
        int64_t capture;
        uint16_t gen;
        char id[SP_OBS_EPISODE_ID_LEN];
        float ema;
        uint32_t seq;
        bool assess_present;
        char assess[96];
        uint8_t sig_count;
    } sample[6];
    uint32_t sample_n;
    /** When true, emit_end re-enters activity END once. */
    bool reenter_end;
    ear_obs_manager_t *reenter_om;
} emit_counts_t;

static void path_join_assessment(
    const ear_cls_assessment_wire_t *w,
    char *out,
    size_t out_len
)
{
    if (out == NULL || out_len == 0u) {
        return;
    }
    out[0] = '\0';
    if (w == NULL || !w->present || w->n_nodes == 0u) {
        return;
    }
    size_t n = 0u;
    for (uint8_t i = 0u; i < w->n_nodes && i < EAR_CLS_PATH_MAX_NODES; i++) {
        if (i > 0u && n + 1u < out_len) {
            out[n++] = '/';
            out[n] = '\0';
        }
        const char *id = w->nodes[i].id;
        while (*id != '\0' && n + 1u < out_len) {
            out[n++] = *id++;
            out[n] = '\0';
        }
    }
}

static emit_counts_t g_emit;

/* Optional recorder used only by the Package 3B fixture. */
typedef struct {
    uint32_t count;
    struct {
        bool start;
        uint64_t candidate_id;
        char id[SP_OBS_EPISODE_ID_LEN];
    } entries[6];
} p3b_events_t;

static void p3b_record(void *ctx, const ear_obs_binding_view_t *binding, bool start)
{
    p3b_events_t *events = ctx;
    if (events == NULL || binding == NULL) {
        return;
    }
    const uint32_t i = events->count++;
    if (i < sizeof(events->entries) / sizeof(events->entries[0])) {
        events->entries[i].start = start;
        events->entries[i].candidate_id = binding->candidate_id;
        memcpy(events->entries[i].id, binding->observation_id, sizeof(events->entries[i].id));
    }
}

static int64_t g_mono_now;
static bool g_mono_pinned;
static int64_t g_start_callback_consume_us;
static int64_t g_clock_after_start_callback;
static bool g_use_start_prepare;
static int g_prepare_builds;
static float g_ring_rms;
static ear_obs_start_measure_t g_prepared;

static void p7_ring_build(
    void *ctx,
    int64_t interval_start_mono_us,
    int64_t capture_mono_us,
    ear_obs_start_aggregates_t *out
)
{
    (void)ctx;
    (void)capture_mono_us;
    g_prepare_builds++;
    memset(out, 0, sizeof(*out));
    out->l0.has_rms_stats = true;
    out->l0.rms_mean_dbfs = g_ring_rms;
    out->l0.window_count = 4u;
    out->have_l1 = true;
    out->l1.window_count = 4u;
    out->l1.interval_start_mono_us = interval_start_mono_us;
    out->have_l2 = true;
    out->l2.window_count = 2u;
}

static bool emit_start(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    const ear_obs_promote_snapshot_t *snap,
    int64_t now_mono_us
)
{
    p3b_record(ctx, binding, true);
    g_emit.starts++;
    g_emit.last_start_cand = binding != NULL ? binding->candidate_id : 0u;
    if (binding != NULL) {
        memcpy(g_emit.last_obs_id, binding->observation_id, sizeof(g_emit.last_obs_id));
        g_emit.last_start_assessment_present = binding->assessment.present;
        path_join_assessment(
            &binding->assessment,
            g_emit.last_start_assessment_path,
            sizeof(g_emit.last_start_assessment_path)
        );
    }
    g_mono_now += g_start_callback_consume_us;
    g_clock_after_start_callback = now_mono_us + g_start_callback_consume_us;
    if (g_use_start_prepare && binding != NULL) {
        bool built = false;
        const ear_obs_start_prepare_status_t st = ear_obs_start_prepare(
            binding->observation_id,
            binding->ep.generation,
            now_mono_us,
            binding->start_mono_us,
            1000000,
            p7_ring_build,
            NULL,
            &g_prepared,
            &built
        );
        if (st != EAR_OBS_START_PREPARE_READY) {
            return false;
        }
        (void)built;
    }
    if (binding != NULL && snap != NULL && g_emit.sample_n < 6u) {
        const uint32_t n = g_emit.sample_n++;
        g_emit.sample[n].capture = now_mono_us;
        g_emit.sample[n].gen = binding->ep.generation;
        memcpy(g_emit.sample[n].id, binding->observation_id, SP_OBS_EPISODE_ID_LEN);
        g_emit.sample[n].ema = snap->ema_rms_dbfs;
        g_emit.sample[n].seq = snap->last_window_seq;
        g_emit.sample[n].sig_count = snap->sig_count;
        g_emit.sample[n].assess_present = binding->assessment.present;
        path_join_assessment(
            &binding->assessment,
            g_emit.sample[n].assess,
            sizeof(g_emit.sample[n].assess)
        );
    }
    if (g_emit.fail_start_remaining > 0u) {
        g_emit.fail_start_remaining--;
        return false;
    }
    return true;
}

static bool emit_periodic(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs
)
{
    (void)ctx;
    g_emit.periodics++;
    g_emit.last_periodic_cand = binding != NULL ? binding->candidate_id : 0u;
    g_emit.last_periodic_start = interval_start_mono_us;
    g_emit.last_periodic_end = interval_end_mono_us;
    g_emit.last_periodic_has_lvl = has_signal_level_dbfs;
    g_emit.last_periodic_lvl = signal_level_dbfs;
    if (binding != NULL) {
        g_emit.last_periodic_assessment_present = binding->assessment.present;
        path_join_assessment(
            &binding->assessment,
            g_emit.last_periodic_assessment_path,
            sizeof(g_emit.last_periodic_assessment_path)
        );
    }
    return true;
}

static bool emit_end(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    ear_obs_end_reason_t reason,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs
)
{
    p3b_record(ctx, binding, false);
    g_emit.ends++;
    g_emit.last_end_cand = binding != NULL ? binding->candidate_id : 0u;
    if (binding != NULL) {
        memcpy(
            g_emit.last_end_obs_id,
            binding->observation_id,
            sizeof(g_emit.last_end_obs_id)
        );
        g_emit.last_end_binding_start_mono_us = binding->start_mono_us;
        if (g_emit.end_obs_n < 8u) {
            memcpy(
                g_emit.end_obs_ids[g_emit.end_obs_n],
                binding->observation_id,
                SP_OBS_EPISODE_ID_LEN
            );
            g_emit.end_obs_n++;
        }
    }
    g_emit.last_end_reason = reason;
    g_emit.last_end_start = interval_start_mono_us;
    g_emit.last_end_end = interval_end_mono_us;
    g_emit.last_end_has_lvl = has_signal_level_dbfs;
    g_emit.last_end_lvl = signal_level_dbfs;

    if (g_emit.reenter_end && g_emit.reenter_om != NULL) {
        g_emit.reenter_end = false;
        ear_obs_manager_on_activity_end_hint(
            g_emit.reenter_om,
            EAR_OBS_ACT_END_AMBIENT,
            interval_start_mono_us,
            interval_end_mono_us,
            has_signal_level_dbfs,
            signal_level_dbfs
        );
    }

    return !g_emit.fail_end;
}

static uint8_t g_uuid_n;

static void fake_uuid(char out[SP_OBS_EPISODE_ID_LEN])
{
    snprintf(
        out,
        SP_OBS_EPISODE_ID_LEN,
        "aaaaaaaa-bbbb-cccc-dddd-%012u",
        (unsigned)g_uuid_n++
    );
}

static void fill_l0(dbmap_l0_window_t *l0, float rms, int64_t t_us)
{
    memset(l0, 0, sizeof(*l0));
    l0->valid = true;
    l0->window_end_mono_us = t_us;
    l0->window_ms = 1000;
    l0->sample_count = 16000;
    l0->rms_dbfs = rms;
    l0->peak_dbfs = rms + 6.0f;
    l0->crest_db = 6.0f;
    l0->band_dbfs[0] = rms - 2.0f;
    l0->band_dbfs[1] = rms;
    l0->band_dbfs[2] = rms - 1.0f;
    l0->band_dbfs[3] = rms - 4.0f;
    l0->hl_valid = true;
    l0->hl_db = 1.0f;
}

static void publish_tone(ae_engine_t *ae, uint32_t seq, float rms)
{
    dbmap_l0_window_t l0;
    fill_l0(&l0, rms, (int64_t)seq * 1000000);
    ae_signature_t sig;
    EXPECT(ae_signature_build(seq, &l0, NULL, NULL, NULL, false, &sig));
    EXPECT(ae_publish_signature(ae, &sig));
}

static void publish_gap(ae_engine_t *ae, uint32_t seq, float rms)
{
    dbmap_l0_window_t l0;
    fill_l0(&l0, rms, (int64_t)seq * 1000000);
    ae_signature_t sig;
    EXPECT(ae_signature_build(seq, &l0, NULL, NULL, NULL, true, &sig));
    EXPECT(sig.quality == AE_QUALITY_GAP);
    EXPECT(ae_publish_signature(ae, &sig));
}

static void clear_classification(ce_engine_t *ce, uint64_t id)
{
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (!ce->slots[i].in_use || ce->slots[i].candidate_id != id) {
            continue;
        }
        memset(&ce->classifications[i], 0, sizeof(ce->classifications[i]));
        return;
    }
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

typedef struct {
    ce_engine_t ce;
    ae_engine_t ae;
    sp_episode_registry_t reg;
    ear_obs_manager_t om;
    ear_promotion_policy_t pol;
} fixture_t;

static int64_t host_mono_now(void *ctx)
{
    (void)ctx;
    return g_mono_now;
}

static void host_obs_poll(ear_obs_manager_t *om, int64_t now_us)
{
    if (!g_mono_pinned) {
        g_mono_now = now_us;
    }
    ear_obs_manager_poll(om, now_us);
}

static void fixture_init(fixture_t *f, bool bind_policy)
{
    memset(f, 0, sizeof(*f));
    memset(&g_emit, 0, sizeof(g_emit));
    g_uuid_n = 1;
    g_mono_now = 0;
    g_mono_pinned = false;

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
    mcfg.emit_start = emit_start;
    mcfg.emit_periodic = emit_periodic;
    mcfg.emit_end = emit_end;
    mcfg.enable_poll_periodic = true;
    mcfg.mono_now_us = host_mono_now;
    mcfg.mono_now_ctx = NULL;
    g_start_callback_consume_us = 0;
    g_clock_after_start_callback = 0;
    g_use_start_prepare = false;
    g_prepare_builds = 0;
    g_ring_rms = 0.0f;
    memset(&g_prepared, 0, sizeof(g_prepared));
    ear_obs_manager_init(&f->om, &mcfg);
    ce_set_promotion_cb(&f->ce, ear_obs_manager_on_promotion, &f->om);

    if (bind_policy) {
        ear_promotion_policy_init(&f->pol, &f->ce, &f->om);
    }
}

static void fixture_fini(fixture_t *f)
{
    ce_stop(&f->ce);
}

/**
 * Stamp experimental AS-001 classification for OM-path tests that bypass
 * Promotion Policy (direct reserve→promote). Production path stamps via
 * fill_snapshot from live CE classification after policy approval.
 */
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

static bool promote_with_reserve(fixture_t *f, uint64_t cid, int64_t now_us)
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
    host_obs_poll(&f->om, now_us);
    return true;
}

/* 1. Candidate not promoted → no Observation */
static void test_no_promote_no_obs(void)
{
    fixture_t f;
    fixture_init(&f, true);
    plant(&f.ce, 10u, AE_CAND_ACTIVE, 5u, -40.0f);
    host_obs_poll(&f.om, 1000);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(g_emit.starts == 0u);
    fixture_fini(&f);
}

/* 2. Reservation fails → no CE promotion */
static void test_reserve_full_blocks_promote(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 1u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 2u, AE_CAND_ACTIVE, 3u, -31.0f);
    plant(&f.ce, 3u, AE_CAND_ACTIVE, 3u, -32.0f);

    EXPECT(promote_with_reserve(&f, 1u, 1000));
    EXPECT(promote_with_reserve(&f, 2u, 2000));
    /* Fill pending pool if not already (2 open → next goes pending). */
    if (ear_obs_manager_n_pending(&f.om) < 2u) {
        EXPECT(promote_with_reserve(&f, 3u, 3000));
    }
    /* Force held+pending = 2 */
    while (ear_obs_manager_n_held(&f.om) + ear_obs_manager_n_pending(&f.om) < 2u) {
        ear_obs_rsv_handle_t h;
        if (ear_obs_manager_reserve(&f.om, &h) != EAR_OBS_RSV_OK) {
            break;
        }
    }

    plant(&f.ce, 4u, AE_CAND_ACTIVE, 9u, -20.0f);
    ear_obs_rsv_handle_t h;
    EXPECT(ear_obs_manager_reserve(&f.om, &h) == EAR_OBS_RSV_FULL);
    /* Policy must not promote when reserve fails — candidate stays ACTIVE. */
    ear_promotion_policy_t pol;
    ear_promotion_policy_init(&pol, &f.ce, &f.om);
    ear_promotion_policy_poll(&pol, 5000);
    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 4u, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    EXPECT(!c.promotion_entered);
    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.reserve_full >= 1u);
    fixture_fini(&f);
}

/* 3. ce_promote fails → reservation released */
static void test_promote_fail_releases_rsv(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 5u, AE_CAND_NASCENT, 1u, -40.0f); /* not promotable */
    ear_obs_rsv_handle_t h;
    EXPECT(ear_obs_manager_reserve(&f.om, &h) == EAR_OBS_RSV_OK);
    EXPECT(ear_obs_manager_n_held(&f.om) == 1u);
    EXPECT(ear_obs_manager_bind_reservation_candidate(&f.om, h, 5u));
    EXPECT(!ce_promote(&f.ce, 5u));
    ear_obs_manager_release_reservation(&f.om, h);
    EXPECT(ear_obs_manager_n_held(&f.om) == 0u);
    ear_obs_manager_release_reservation(&f.om, h); /* idempotent */
    EXPECT(ear_obs_manager_n_held(&f.om) == 0u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    fixture_fini(&f);
}

/* 4. Successful ENTER → exactly one Observation */
static void test_enter_one_obs(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 7u, AE_CAND_ACTIVE, 4u, -35.0f);
    EXPECT(promote_with_reserve(&f, 7u, 5000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(ear_obs_manager_n_pending(&f.om) == 0u);
    EXPECT(ear_obs_manager_n_held(&f.om) == 0u);
    EXPECT(g_emit.starts == 1u);
    EXPECT(g_emit.last_start_cand == 7u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 1u);
    fixture_fini(&f);
}

/* 5. Duplicate ENTER → no duplicate allocation */
static void test_dup_enter(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 8u, AE_CAND_ACTIVE, 4u, -35.0f);
    EXPECT(promote_with_reserve(&f, 8u, 5000));
    EXPECT(g_emit.starts == 1u);
    /* Inject duplicate ENTER */
    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 8u, &c));
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_ENTER, &f.om);
    host_obs_poll(&f.om, 6000);
    EXPECT(g_emit.starts == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    fixture_fini(&f);
}

/* 6–7. Two open → third pending; two pending → reserve denied */
static void test_capacity_pending(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 11u, AE_CAND_ACTIVE, 5u, -30.0f);
    plant(&f.ce, 12u, AE_CAND_ACTIVE, 5u, -31.0f);
    plant(&f.ce, 13u, AE_CAND_ACTIVE, 5u, -32.0f);
    plant(&f.ce, 14u, AE_CAND_ACTIVE, 5u, -33.0f);

    EXPECT(promote_with_reserve(&f, 11u, 1000));
    EXPECT(promote_with_reserve(&f, 12u, 2000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);
    EXPECT(promote_with_reserve(&f, 13u, 3000));
    EXPECT(ear_obs_manager_n_pending(&f.om) == 1u);
    EXPECT(promote_with_reserve(&f, 14u, 4000));
    EXPECT(ear_obs_manager_n_pending(&f.om) == 2u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);

    ear_obs_rsv_handle_t h;
    EXPECT(ear_obs_manager_reserve(&f.om, &h) == EAR_OBS_RSV_FULL);
    fixture_fini(&f);
}

/* 8. Held counts against pending capacity */
static void test_held_counts(void)
{
    fixture_t f;
    fixture_init(&f, false);
    ear_obs_rsv_handle_t h1;
    ear_obs_rsv_handle_t h2;
    EXPECT(ear_obs_manager_reserve(&f.om, &h1) == EAR_OBS_RSV_OK);
    EXPECT(ear_obs_manager_reserve(&f.om, &h2) == EAR_OBS_RSV_OK);
    EXPECT(ear_obs_manager_n_held(&f.om) == 2u);
    ear_obs_rsv_handle_t h3;
    EXPECT(ear_obs_manager_reserve(&f.om, &h3) == EAR_OBS_RSV_FULL);
    ear_obs_manager_release_reservation(&f.om, h1);
    EXPECT(ear_obs_manager_n_held(&f.om) == 1u);
    EXPECT(ear_obs_manager_reserve(&f.om, &h3) == EAR_OBS_RSV_OK);
    fixture_fini(&f);
}

/* 9–10. Serialized promote cannot overbook; dup release/commit safe */
static void test_no_overbook_and_dup_release(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 20u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 21u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 22u, AE_CAND_ACTIVE, 3u, -30.0f);

    EXPECT(promote_with_reserve(&f, 20u, 1000));
    EXPECT(promote_with_reserve(&f, 21u, 2000));
    EXPECT(promote_with_reserve(&f, 22u, 3000));
    EXPECT(ear_obs_manager_n_open(&f.om) + ear_obs_manager_n_pending(&f.om) <= 4u);
    EXPECT(ear_obs_manager_n_held(&f.om) + ear_obs_manager_n_pending(&f.om) <= 2u);
    EXPECT(ear_obs_manager_n_open(&f.om) <= 2u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == ear_obs_manager_n_open(&f.om));

    ear_obs_rsv_handle_t junk = {.magic = 0xE0B50001u, .index = 0, .generation = 99};
    ear_obs_manager_release_reservation(&f.om, junk);
    EXPECT(ear_obs_manager_commit_reservation(&f.om, junk, 99u, NULL, 0) == false);
    fixture_fini(&f);
}

/* 11. Engine stop releases HELD/PENDING */
static void test_engine_stop(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 30u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 31u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 32u, AE_CAND_ACTIVE, 3u, -30.0f);
    EXPECT(promote_with_reserve(&f, 30u, 1000));
    EXPECT(promote_with_reserve(&f, 31u, 2000));
    EXPECT(promote_with_reserve(&f, 32u, 3000));
    EXPECT(ear_obs_manager_n_pending(&f.om) >= 1u);

    ear_obs_rsv_handle_t h;
    (void)ear_obs_manager_reserve(&f.om, &h); /* may fail if pool full */

    ce_stop(&f.ce);
    host_obs_poll(&f.om, 9000); /* drain EXIT */
    ear_obs_manager_on_engine_stop(&f.om, 9000);
    EXPECT(ear_obs_manager_n_held(&f.om) == 0u);
    EXPECT(ear_obs_manager_n_pending(&f.om) == 0u);
    /* Open Obs remain (not auto-ended). */
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);
    EXPECT(g_emit.ends == 0u);
}

/* 12–13. FIFO: pending gets freed slot before new promote; order preserved */
static void test_fifo_drain_order(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 40u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 41u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 42u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 43u, AE_CAND_ACTIVE, 3u, -30.0f);

    EXPECT(promote_with_reserve(&f, 40u, 1000)); /* open */
    EXPECT(promote_with_reserve(&f, 41u, 2000)); /* open */
    EXPECT(promote_with_reserve(&f, 42u, 3000)); /* pending A */
    EXPECT(promote_with_reserve(&f, 43u, 4000)); /* pending B */
    EXPECT(ear_obs_manager_n_pending(&f.om) == 2u);
    EXPECT(f.om.pending[f.om.pending_order[0]].candidate_id == 42u);
    EXPECT(f.om.pending[f.om.pending_order[1]].candidate_id == 43u);

    /* End obs for 40 → A (42) must take slot before any new promote. */
    ear_obs_manager_force_end_all(&f.om, EAR_OBS_END_EXPLICIT, 5000);
    /* force_end_all ends ALL open — both 40 and 41. Drain should admit 42 then 43. */
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);
    EXPECT(ear_obs_manager_n_pending(&f.om) == 0u);
    EXPECT(g_emit.starts >= 4u);
    /* First drained pending was 42 */
    EXPECT(f.om.bind[0].in_use || f.om.bind[1].in_use);
    bool have42 = false;
    bool have43 = false;
    for (uint8_t i = 0; i < 2; i++) {
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == 42u) {
            have42 = true;
        }
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == 43u) {
            have43 = true;
        }
    }
    EXPECT(have42 && have43);
    fixture_fini(&f);
}

/** Free one open binding without activity END (FIFO isolation helper). */
static void free_binding_for_candidate(fixture_t *f, uint64_t cid)
{
    for (uint8_t i = 0; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (f->om.bind[i].in_use && f->om.bind[i].candidate_id == cid) {
            sp_episode_registry_release(&f->reg, f->om.bind[i].ep);
            memset(&f->om.bind[i], 0, sizeof(f->om.bind[i]));
            return;
        }
    }
    EXPECT(0 && "binding not found");
}

/* 12 variant: free one slot; pending A before new B promote */
static void test_fifo_before_new_promote(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 50u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 51u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 52u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 53u, AE_CAND_ACTIVE, 3u, -30.0f);

    EXPECT(promote_with_reserve(&f, 50u, 1000));
    EXPECT(promote_with_reserve(&f, 51u, 2000));
    EXPECT(promote_with_reserve(&f, 52u, 3000)); /* pending */
    EXPECT(ear_obs_manager_n_pending(&f.om) == 1u);

    /* Free only candidate 50 so FIFO drain can admit 52 without global END. */
    free_binding_for_candidate(&f, 50u);
    host_obs_poll(&f.om, 3500);
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u); /* 51 + drained 52 */
    EXPECT(ear_obs_manager_n_pending(&f.om) == 0u);
    bool have52 = false;
    for (uint8_t i = 0; i < 2; i++) {
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == 52u) {
            have52 = true;
        }
    }
    EXPECT(have52);

    /* New promote 53 should not steal ahead of drained 52 (already bound). */
    EXPECT(promote_with_reserve(&f, 53u, 4000));
    EXPECT(ear_obs_manager_n_pending(&f.om) == 1u);
    EXPECT(f.om.pending[f.om.pending_order[0]].candidate_id == 53u);
    fixture_fini(&f);
}

/* 14. Alloc failure at head does not skip (registry full, manager open < 2) */
static void test_fifo_no_skip_on_alloc_fail(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 60u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 62u, AE_CAND_ACTIVE, 3u, -30.0f);

    EXPECT(promote_with_reserve(&f, 60u, 1000)); /* open 1 */
    EXPECT(promote_with_reserve(&f, 62u, 2000)); /* open 2 */
    /* Free manager binding for 60 but keep its registry slot occupied. */
    for (uint8_t i = 0; i < 2; i++) {
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == 60u) {
            memset(&f.om.bind[i], 0, sizeof(f.om.bind[i]));
        }
    }
    /* End 62's obs via hint so 62 becomes pending? Better: promote 63 as pending
     * while registry still full (2 slots: 60 orphan + 62). */
    plant(&f.ce, 63u, AE_CAND_ACTIVE, 3u, -30.0f);
    /* Free binding 62 without releasing registry — both slots still in use. */
    for (uint8_t i = 0; i < 2; i++) {
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == 62u) {
            memset(&f.om.bind[i], 0, sizeof(f.om.bind[i]));
        }
    }
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 2u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);

    /* Commit 63 as pending via reserve/promote while registry full → pending. */
    EXPECT(promote_with_reserve(&f, 63u, 3000));
    EXPECT(ear_obs_manager_n_pending(&f.om) == 1u);
    EXPECT(f.om.pending[f.om.pending_order[0]].candidate_id == 63u);

    plant(&f.ce, 64u, AE_CAND_ACTIVE, 3u, -30.0f);
    EXPECT(promote_with_reserve(&f, 64u, 4000));
    EXPECT(ear_obs_manager_n_pending(&f.om) == 2u);
    EXPECT(f.om.pending[f.om.pending_order[0]].candidate_id == 63u);

    /* Drain attempts: registry full → fail, do not skip to 64. */
    host_obs_poll(&f.om, 5000);
    EXPECT(ear_obs_manager_n_pending(&f.om) == 2u);
    EXPECT(f.om.pending[f.om.pending_order[0]].candidate_id == 63u);
    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.drain_alloc_fail >= 1u);
    fixture_fini(&f);
}

/* 15. Pending cancel allows next to advance */
static void test_pending_cancel_advances(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 70u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 71u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 72u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 73u, AE_CAND_ACTIVE, 3u, -30.0f);

    EXPECT(promote_with_reserve(&f, 70u, 1000));
    EXPECT(promote_with_reserve(&f, 71u, 2000));
    EXPECT(promote_with_reserve(&f, 72u, 3000));
    EXPECT(promote_with_reserve(&f, 73u, 4000));

    /* EXIT on pending head 72 */
    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 72u, &c));
    EXPECT(ce_reject(&f.ce, 72u)); /* emits EXIT */
    host_obs_poll(&f.om, 4500);
    EXPECT(ear_obs_manager_n_pending(&f.om) == 1u);
    EXPECT(f.om.pending[f.om.pending_order[0]].candidate_id == 73u);

    /* Free one slot → 73 advances (activity END would close both). */
    free_binding_for_candidate(&f, 70u);
    host_obs_poll(&f.om, 5000);
    bool have73 = false;
    for (uint8_t i = 0; i < 2; i++) {
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == 73u) {
            have73 = true;
        }
    }
    EXPECT(have73);
    fixture_fini(&f);
}

/* 16. Poll recovers blocked FIFO head after slot free */
static void test_poll_recovers_fifo(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 80u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 81u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 82u, AE_CAND_ACTIVE, 3u, -30.0f);
    EXPECT(promote_with_reserve(&f, 80u, 1000));
    EXPECT(promote_with_reserve(&f, 81u, 2000));
    EXPECT(promote_with_reserve(&f, 82u, 3000));

    /* Manually release one registry slot + binding without drain */
    for (uint8_t i = 0; i < 2; i++) {
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == 80u) {
            sp_episode_registry_release(&f.reg, f.om.bind[i].ep);
            memset(&f.om.bind[i], 0, sizeof(f.om.bind[i]));
        }
    }
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(ear_obs_manager_n_pending(&f.om) == 1u);
    host_obs_poll(&f.om, 4000); /* recovery drain */
    EXPECT(ear_obs_manager_n_pending(&f.om) == 0u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);
    fixture_fini(&f);
}

/* 17–18. EXIT detaches, does not END; evidence after EXIT dropped */
static void test_exit_detach_no_end(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 90u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 90u, 1000));
    EXPECT(ear_obs_manager_binding_ce_live(&f.om, 90u));

    EXPECT(ce_reject(&f.ce, 90u));
    host_obs_poll(&f.om, 2000);
    EXPECT(!ear_obs_manager_binding_ce_live(&f.om, 90u));
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(g_emit.ends == 0u);

    ae_candidate_t ghost;
    memset(&ghost, 0, sizeof(ghost));
    ghost.candidate_id = 90u;
    ghost.evidence_count = 99u;
    ear_obs_manager_diag_t d0;
    ear_obs_manager_get_diag(&f.om, &d0);
    ear_obs_manager_on_candidate_evidence(&f.om, 90u, NULL, &ghost);
    ear_obs_manager_diag_t d1;
    ear_obs_manager_get_diag(&f.om, &d1);
    EXPECT(d1.evidence_dropped == d0.evidence_dropped + 1u);
    fixture_fini(&f);
}

/* 19–22. Timeout / EXIT idempotency */
static void test_pending_timeout_and_sync_exit(void)
{
    fixture_t f;
    fixture_init(&f, false);
    /* Short timeout via re-init config */
    f.om.cfg.pending_timeout_ms = 1000u;

    plant(&f.ce, 100u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 101u, AE_CAND_ACTIVE, 3u, -30.0f);
    plant(&f.ce, 102u, AE_CAND_ACTIVE, 3u, -30.0f);
    EXPECT(promote_with_reserve(&f, 100u, 0));
    EXPECT(promote_with_reserve(&f, 101u, 0));
    EXPECT(promote_with_reserve(&f, 102u, 0));
    EXPECT(ear_obs_manager_n_pending(&f.om) == 1u);

    /* Timeout → cancel then ce_reject → sync EXIT must not double-free */
    host_obs_poll(&f.om, 2000000); /* 2s > 1s timeout */
    EXPECT(ear_obs_manager_n_pending(&f.om) == 0u);
    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.pending_timeout >= 1u);

    /* Duplicate timeout/poll harmless */
    host_obs_poll(&f.om, 3000000);
    ear_obs_manager_get_diag(&f.om, &d);
    const uint32_t t1 = d.pending_timeout;
    host_obs_poll(&f.om, 4000000);
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.pending_timeout == t1);

    /* Duplicate EXIT */
    ae_candidate_t c;
    memset(&c, 0, sizeof(c));
    c.candidate_id = 102u;
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, 5000000);
    EXPECT(ear_obs_manager_n_pending(&f.om) == 0u);
    fixture_fini(&f);
}

/* Test D: END + registry release exactly once (including while ce_live). */
static void test_end_once(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 110u, AE_CAND_ACTIVE, 3u, -30.0f);
    EXPECT(promote_with_reserve(&f, 110u, 1000));
    EXPECT(ear_obs_manager_binding_ce_live(&f.om, 110u));
    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 1500, 2000, true, -38.0f);
    EXPECT(g_emit.ends == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);
    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 2500, 3000, true, -38.0f);
    EXPECT(g_emit.ends == 1u);
    fixture_fini(&f);
}

/* Test A: RMS END before CE EXIT → immediate END; EXIT is no-op */
static void test_end_order_a_end_before_exit(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 400u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 400u, 1000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(ear_obs_manager_binding_ce_live(&f.om, 400u));
    char obs_id[SP_OBS_EPISODE_ID_LEN];
    memcpy(obs_id, g_emit.last_obs_id, sizeof(obs_id));

    /* Activity END terminates immediately while ce_live remains true. */
    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 1800, 2000, true, -45.0f);
    EXPECT(g_emit.ends == 1u);
    EXPECT(g_emit.last_end_reason == EAR_OBS_END_HINT_ACTIVITY);
    EXPECT(g_emit.last_end_cand == 400u);
    EXPECT(strncmp(g_emit.last_end_obs_id, obs_id, SP_OBS_EPISODE_ID_LEN) == 0);
    EXPECT(g_emit.last_end_start == 1800);
    EXPECT(g_emit.last_end_end == 2000);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);
    EXPECT(!ear_obs_manager_binding_end_hint_latched(&f.om, 400u));

    /* Repeated END: no second emission. */
    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 1900, 2100, true, -46.0f);
    EXPECT(g_emit.ends == 1u);

    /* EXIT after END: detach-only no-op (binding already gone). */
    EXPECT(ce_reject(&f.ce, 400u));
    host_obs_poll(&f.om, 2200);
    EXPECT(g_emit.ends == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);

    ae_candidate_t c;
    memset(&c, 0, sizeof(c));
    c.candidate_id = 400u;
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, 2300);
    EXPECT(g_emit.ends == 1u);
    fixture_fini(&f);
}

/* Order B: CE EXIT before RMS END → END on hint */
static void test_end_order_b_exit_before_end(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 401u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 401u, 1000));
    EXPECT(ce_reject(&f.ce, 401u));
    host_obs_poll(&f.om, 1500);
    EXPECT(!ear_obs_manager_binding_ce_live(&f.om, 401u));
    EXPECT(g_emit.ends == 0u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);

    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 1600, 1700, true, -44.0f);
    EXPECT(g_emit.ends == 1u);
    EXPECT(g_emit.last_end_start == 1600);
    EXPECT(g_emit.last_end_end == 1700);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);

    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 1800, 1900, true, -44.0f);
    EXPECT(g_emit.ends == 1u);
    fixture_fini(&f);
}

/* EXIT alone never ENDs */
static void test_exit_alone_no_end(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 402u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 402u, 1000));
    EXPECT(ce_reject(&f.ce, 402u));
    host_obs_poll(&f.om, 1500);
    EXPECT(g_emit.ends == 0u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 1u);
    fixture_fini(&f);
}

/* PERIODIC activity hint preserves interval metadata for reporting */
static void test_periodic_hint_reporting_fields(void)
{
    fixture_t f;
    fixture_init(&f, false);
    f.om.cfg.enable_poll_periodic = false;
    plant(&f.ce, 410u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 410u, 1000));

    ear_obs_manager_on_activity_periodic_hint(&f.om, 1000, 6000, true, -36.5f);
    EXPECT(g_emit.periodics == 1u);
    EXPECT(g_emit.last_periodic_start == 1000);
    EXPECT(g_emit.last_periodic_end == 6000);
    EXPECT(g_emit.last_periodic_has_lvl);
    EXPECT(g_emit.last_periodic_lvl == -36.5f);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    fixture_fini(&f);
}

/**
 * Two open Obs, mixed ce_live: one global RMS END ends both immediately.
 */
static void test_two_open_mixed_ce_live_shared_end(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 420u, AE_CAND_ACTIVE, 5u, -30.0f);
    plant(&f.ce, 421u, AE_CAND_ACTIVE, 4u, -31.0f);
    EXPECT(promote_with_reserve(&f, 420u, 1000));
    EXPECT(promote_with_reserve(&f, 421u, 2000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);

    /* Detach only 420 (CE EXIT without END). */
    EXPECT(ce_reject(&f.ce, 420u));
    host_obs_poll(&f.om, 2500);
    EXPECT(!ear_obs_manager_binding_ce_live(&f.om, 420u));
    EXPECT(ear_obs_manager_binding_ce_live(&f.om, 421u));
    EXPECT(g_emit.ends == 0u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);

    /* Global RMS END: both open bindings terminate immediately. */
    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 2600, 2700, true, -48.0f);
    EXPECT(g_emit.ends == 2u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);

    /* Later EXIT for 421: no extra END. */
    EXPECT(ce_reject(&f.ce, 421u));
    host_obs_poll(&f.om, 2800);
    EXPECT(g_emit.ends == 2u);
    fixture_fini(&f);
}

/**
 * Two open Obs share one RMS PERIODIC: duplicate interval metadata on both
 * (global acoustic stream → both observation_ids).
 */
static void test_two_open_shared_periodic_duplicated(void)
{
    fixture_t f;
    fixture_init(&f, false);
    f.om.cfg.enable_poll_periodic = false;
    plant(&f.ce, 430u, AE_CAND_ACTIVE, 5u, -30.0f);
    plant(&f.ce, 431u, AE_CAND_ACTIVE, 4u, -31.0f);
    EXPECT(promote_with_reserve(&f, 430u, 1000));
    EXPECT(promote_with_reserve(&f, 431u, 2000));

    ear_obs_manager_on_activity_periodic_hint(&f.om, 2000, 7000, true, -34.0f);
    EXPECT(g_emit.periodics == 2u);
    EXPECT(g_emit.last_periodic_start == 2000);
    EXPECT(g_emit.last_periodic_end == 7000);
    EXPECT(g_emit.last_periodic_lvl == -34.0f);
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);
    fixture_fini(&f);
}

/** Test B: both live — one activity END ends both immediately. */
static void test_two_open_both_live_end_latches_both(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 440u, AE_CAND_ACTIVE, 5u, -30.0f);
    plant(&f.ce, 441u, AE_CAND_ACTIVE, 4u, -31.0f);
    EXPECT(promote_with_reserve(&f, 440u, 1000));
    char obs_a[SP_OBS_EPISODE_ID_LEN];
    memcpy(obs_a, g_emit.last_obs_id, sizeof(obs_a));
    EXPECT(promote_with_reserve(&f, 441u, 2000));
    char obs_b[SP_OBS_EPISODE_ID_LEN];
    memcpy(obs_b, g_emit.last_obs_id, sizeof(obs_b));
    EXPECT(strncmp(obs_a, obs_b, SP_OBS_EPISODE_ID_LEN) != 0);

    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 3000, 3100, true, -50.0f);
    EXPECT(g_emit.ends == 2u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);
    EXPECT(g_emit.end_obs_n == 2u);

    EXPECT(ce_reject(&f.ce, 440u));
    host_obs_poll(&f.om, 3200);
    EXPECT(ce_reject(&f.ce, 441u));
    host_obs_poll(&f.om, 3300);
    EXPECT(g_emit.ends == 2u);
    fixture_fini(&f);
}

/* Test E: rematch after END does not revive Observation */
static void test_rematch_after_end_no_revive(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 450u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 450u, 1000));
    char obs_id[SP_OBS_EPISODE_ID_LEN];
    memcpy(obs_id, g_emit.last_obs_id, sizeof(obs_id));

    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 2000, 2100, true, -50.0f);
    EXPECT(g_emit.ends == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);

    /* Candidate still ACTIVE/promoted in CE; rematch path does not reopen Obs. */
    ae_candidate_t live;
    EXPECT(ce_find_candidate(&f.ce, 450u, &live));
    EXPECT(live.promotion_entered);
    host_obs_poll(&f.om, 2200);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(g_emit.starts == 1u);
    EXPECT(g_emit.ends == 1u);
    EXPECT(strncmp(g_emit.last_end_obs_id, obs_id, SP_OBS_EPISODE_ID_LEN) == 0);
    fixture_fini(&f);
}

/* Test F: new promotion after END; stale EXIT cannot affect new Obs */
static void test_new_promote_after_end_stale_exit_safe(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 460u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 460u, 1000));
    char old_obs[SP_OBS_EPISODE_ID_LEN];
    memcpy(old_obs, g_emit.last_obs_id, sizeof(old_obs));
    const uint32_t starts_before = g_emit.starts;

    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 2000, 2100, true, -50.0f);
    EXPECT(g_emit.ends == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(ear_obs_manager_n_held(&f.om) == 0u);

    /* Reject old candidate so a fresh candidate can be promoted. */
    EXPECT(ce_reject(&f.ce, 460u));
    host_obs_poll(&f.om, 2200);

    plant(&f.ce, 461u, AE_CAND_ACTIVE, 5u, -28.0f);
    EXPECT(promote_with_reserve(&f, 461u, 3000));
    EXPECT(g_emit.starts == starts_before + 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    char new_obs[SP_OBS_EPISODE_ID_LEN];
    memcpy(new_obs, g_emit.last_obs_id, sizeof(new_obs));
    EXPECT(strncmp(old_obs, new_obs, SP_OBS_EPISODE_ID_LEN) != 0);
    EXPECT(ear_obs_manager_binding_ce_live(&f.om, 461u));

    /* Stale EXIT for old candidate 460 must not end or detach 461. */
    ae_candidate_t stale;
    memset(&stale, 0, sizeof(stale));
    stale.candidate_id = 460u;
    ear_obs_manager_on_promotion(&stale, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, 3100);
    EXPECT(g_emit.ends == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(ear_obs_manager_binding_ce_live(&f.om, 461u));
    EXPECT(strncmp(g_emit.last_obs_id, new_obs, SP_OBS_EPISODE_ID_LEN) == 0);
    fixture_fini(&f);
}

/* Test H: capacity recovery / reserve-full regression */
static void test_capacity_recovery_after_end(void)
{
    fixture_t f;
    fixture_init(&f, true);
    plant(&f.ce, 470u, AE_CAND_ACTIVE, 5u, -30.0f);
    plant(&f.ce, 471u, AE_CAND_ACTIVE, 4u, -31.0f);
    plant(&f.ce, 472u, AE_CAND_ACTIVE, 6u, -29.0f);
    plant(&f.ce, 473u, AE_CAND_ACTIVE, 3u, -32.0f);

    EXPECT(promote_with_reserve(&f, 470u, 1000));
    EXPECT(promote_with_reserve(&f, 471u, 2000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);

    /* Further promotes fill pending / hit reserve-full under capacity pressure. */
    EXPECT(promote_with_reserve(&f, 472u, 3000));
    EXPECT(ear_obs_manager_n_pending(&f.om) >= 1u);
    ear_obs_rsv_handle_t h;
    const ear_obs_rsv_result_t rr = ear_obs_manager_reserve(&f.om, &h);
    if (rr == EAR_OBS_RSV_FULL) {
        ear_obs_manager_diag_t d;
        ear_obs_manager_get_diag(&f.om, &d);
        EXPECT(d.reserve_full >= 1u);
    } else {
        ear_obs_manager_release_reservation(&f.om, h);
    }

    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 4000, 4100, true, -55.0f);
    EXPECT(g_emit.ends == 2u);
    /* Pending may drain into freed slots. */
    EXPECT(ear_obs_manager_n_open(&f.om) <= 2u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == ear_obs_manager_n_open(&f.om));

    /* Force-clear remaining so capacity is fully free for a fresh promote. */
    ear_obs_manager_force_end_all(&f.om, EAR_OBS_END_EXPLICIT, 4200);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(ear_obs_manager_n_pending(&f.om) == 0u);

    plant(&f.ce, 474u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 474u, 5000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(g_emit.starts >= 3u);
    fixture_fini(&f);
}

/* Stuck-binding regression: END without EXIT recovers capacity for policy. */
static void test_end_without_exit_unsticks_bindings(void)
{
    fixture_t f;
    fixture_init(&f, true);
    plant(&f.ce, 480u, AE_CAND_ACTIVE, 5u, -30.0f);
    plant(&f.ce, 481u, AE_CAND_ACTIVE, 4u, -31.0f);
    EXPECT(promote_with_reserve(&f, 480u, 1000));
    EXPECT(promote_with_reserve(&f, 481u, 2000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);

    /* Candidates remain live; no EXIT. Activity END must still free capacity. */
    EXPECT(ear_obs_manager_binding_ce_live(&f.om, 480u));
    EXPECT(ear_obs_manager_binding_ce_live(&f.om, 481u));
    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 3000, 3100, true, -52.0f);
    EXPECT(g_emit.ends == 2u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(ear_obs_manager_n_held(&f.om) == 0u);

    plant(&f.ce, 482u, AE_CAND_ACTIVE, 6u, -28.0f);
    mark_as001_relevant(&f.ce, 482u);
    ear_promotion_policy_poll(&f.pol, 4000);
    host_obs_poll(&f.om, 4000);
    EXPECT(ear_obs_manager_n_open(&f.om) >= 1u);
    fixture_fini(&f);
}

static int find_bind_idx(const fixture_t *f, uint64_t cid)
{
    for (uint8_t i = 0; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (f->om.bind[i].in_use && f->om.bind[i].candidate_id == cid) {
            return (int)i;
        }
    }
    return -1;
}

static void fill_l0_shape(
    dbmap_l0_window_t *l0,
    float rms,
    const float band_rel[AE_BAND_COUNT],
    int64_t t_us
)
{
    memset(l0, 0, sizeof(*l0));
    l0->valid = true;
    l0->window_end_mono_us = t_us;
    l0->window_ms = 1000;
    l0->sample_count = 16000;
    l0->rms_dbfs = rms;
    l0->peak_dbfs = rms + 6.0f;
    l0->crest_db = 6.0f;
    for (uint32_t i = 0; i < AE_BAND_COUNT; i++) {
        l0->band_dbfs[i] = rms + band_rel[i];
    }
    l0->hl_valid = true;
    l0->hl_db = 1.0f;
}

static void fill_l1(
    dbmap_l1_window_t *l1,
    float dominant_hz,
    float flatness,
    float centroid_hz,
    int64_t t_us
)
{
    memset(l1, 0, sizeof(*l1));
    l1->valid = true;
    l1->window_end_mono_us = t_us;
    l1->window_ms = 1000;
    l1->dominant_hz = dominant_hz;
    l1->flatness = flatness;
    l1->centroid_hz = centroid_hz;
    l1->bandwidth_hz = 100.0f;
}

/** Publish a shaped signature through AE→CE (n_enter=1 → ACTIVE on first). */
static void publish_shaped(
    fixture_t *f,
    uint32_t seq,
    float rms,
    const float band_rel[AE_BAND_COUNT],
    float dominant_hz
)
{
    dbmap_l0_window_t l0;
    dbmap_l1_window_t l1;
    const int64_t t = (int64_t)seq * 1000000;
    fill_l0_shape(&l0, rms, band_rel, t);
    fill_l1(&l1, dominant_hz, 0.2f, dominant_hz + 10.0f, t);
    ae_signature_t sig;
    EXPECT(ae_signature_build(seq, &l0, &l1, NULL, NULL, false, &sig));
    EXPECT(ae_publish_signature(&f->ae, &sig));
}

static const float k_band_a[AE_BAND_COUNT] = { -1.0f, 2.0f, 0.0f, -3.0f };
static const float k_band_b[AE_BAND_COUNT] = { 4.0f, -2.0f, -4.0f, 3.0f };

/* --- CE Stage 2: post-promotion signature evidence --- */

/** 1. Post-promote feed adds candidate-associated signature copies. */
static void test_s2_post_promote_adds_sig_copies(void)
{
    fixture_t f;
    fixture_init(&f, false);

    publish_shaped(&f, 1u, -40.0f, k_band_a, 120.0f);
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&f.ce, 0u, &c));
    const uint64_t cid = c.candidate_id;
    EXPECT(promote_with_reserve(&f, cid, 1000));
    const int bi = find_bind_idx(&f, cid);
    EXPECT(bi >= 0);
    const uint8_t enter_n = f.om.bind[bi].snap.sig_count;
    EXPECT(enter_n >= 1u);
    const uint32_t enter_top = f.om.bind[bi].snap.sigs[0].window_seq;

    publish_shaped(&f, 2u, -39.0f, k_band_a, 121.0f);
    ae_signature_t last;
    EXPECT(ce_get_last_evidence_signature(&f.ce, cid, &last));
    EXPECT(last.window_seq == 2u);

    ear_obs_manager_feed_from_ce(&f.om, &f.ce);
    EXPECT(f.om.bind[bi].snap.sigs[0].window_seq == 2u);
    EXPECT(f.om.bind[bi].snap.sigs[0].rms_dbfs == last.rms_dbfs);
    EXPECT(f.om.bind[bi].last_owned_sig_window_seq == 2u);
    if (enter_n < EAR_OBS_PROMOTE_SIG_CAP) {
        EXPECT(f.om.bind[bi].snap.sig_count == enter_n + 1u);
    } else {
        EXPECT(f.om.bind[bi].snap.sig_count == EAR_OBS_PROMOTE_SIG_CAP);
        EXPECT(enter_top != 2u || f.om.bind[bi].snap.sigs[0].window_seq == 2u);
    }
    fixture_fini(&f);
}

/** 2. Two candidates cannot cross-contaminate Observation evidence rings. */
static void test_s2_two_candidates_no_cross_contaminate(void)
{
    fixture_t f;
    fixture_init(&f, false);

    publish_shaped(&f, 1u, -40.0f, k_band_a, 100.0f);
    publish_shaped(&f, 2u, -40.0f, k_band_b, 400.0f);
    EXPECT(ce_candidate_count(&f.ce) == 2u);

    ae_candidate_t ca;
    ae_candidate_t cb;
    EXPECT(ce_get_candidate(&f.ce, 0u, &ca));
    EXPECT(ce_get_candidate(&f.ce, 1u, &cb));
    EXPECT(ca.candidate_id != cb.candidate_id);
    EXPECT(promote_with_reserve(&f, ca.candidate_id, 3000));
    EXPECT(promote_with_reserve(&f, cb.candidate_id, 4000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);

    publish_shaped(&f, 3u, -38.0f, k_band_a, 105.0f);
    publish_shaped(&f, 4u, -38.0f, k_band_b, 410.0f);
    ear_obs_manager_feed_from_ce(&f.om, &f.ce);

    const int ia = find_bind_idx(&f, ca.candidate_id);
    const int ib = find_bind_idx(&f, cb.candidate_id);
    EXPECT(ia >= 0 && ib >= 0);
    EXPECT(f.om.bind[ia].snap.sigs[0].window_seq == 3u);
    EXPECT(f.om.bind[ib].snap.sigs[0].window_seq == 4u);
    EXPECT(f.om.bind[ia].snap.sigs[0].window_seq
           != f.om.bind[ib].snap.sigs[0].window_seq);
    fixture_fini(&f);
}

/** 3. AE history overwrite does not mutate already-owned Observation evidence. */
static void test_s2_ae_history_overwrite_safe(void)
{
    fixture_t f;
    fixture_init(&f, false);

    publish_shaped(&f, 1u, -40.0f, k_band_a, 120.0f);
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&f.ce, 0u, &c));
    const uint64_t cid = c.candidate_id;
    EXPECT(promote_with_reserve(&f, cid, 1000));

    publish_shaped(&f, 2u, -37.0f, k_band_a, 122.0f);
    ear_obs_manager_feed_from_ce(&f.om, &f.ce);
    const int bi = find_bind_idx(&f, cid);
    EXPECT(bi >= 0);
    const float owned_rms = f.om.bind[bi].snap.sigs[0].rms_dbfs;
    const uint32_t owned_seq = f.om.bind[bi].snap.sigs[0].window_seq;
    EXPECT(owned_seq == 2u);

    /* Flood AE history with unrelated windows (overwrite ring). */
    for (uint32_t s = 3u; s < 40u; s++) {
        publish_tone(&f.ae, s, -90.0f);
    }

    EXPECT(f.om.bind[bi].snap.sigs[0].window_seq == owned_seq);
    EXPECT(f.om.bind[bi].snap.sigs[0].rms_dbfs == owned_rms);
    fixture_fini(&f);
}

/** 4. INVALID and GAP must not add signature copies. */
static void test_s2_gap_invalid_rejected(void)
{
    fixture_t f;
    fixture_init(&f, false);

    publish_shaped(&f, 1u, -40.0f, k_band_a, 120.0f);
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&f.ce, 0u, &c));
    const uint64_t cid = c.candidate_id;
    EXPECT(promote_with_reserve(&f, cid, 1000));
    const int bi = find_bind_idx(&f, cid);
    EXPECT(bi >= 0);
    const uint8_t n0 = f.om.bind[bi].snap.sig_count;
    const uint32_t top0 = f.om.bind[bi].last_owned_sig_window_seq;

    ear_obs_manager_diag_t d0;
    ear_obs_manager_get_diag(&f.om, &d0);

    /* Direct GAP offer with matching cand fields (bypass CE GAP coast). */
    ae_signature_t gap;
    memset(&gap, 0, sizeof(gap));
    gap.schema_version = AE_SIGNATURE_SCHEMA_VERSION;
    gap.window_seq = top0 + 1u;
    gap.window_end_mono_us = (int64_t)gap.window_seq * 1000000;
    gap.window_ms = 1000;
    gap.quality = AE_QUALITY_GAP;
    gap.rms_dbfs = -40.0f;
    ae_candidate_t live;
    EXPECT(ce_find_candidate(&f.ce, cid, &live));
    live.last_window_seq = gap.window_seq;
    live.promotion_gen = f.om.bind[bi].ce_promotion_gen;
    ear_obs_manager_on_candidate_evidence(&f.om, cid, &gap, &live);

    ae_signature_t inv = gap;
    inv.window_seq = top0 + 2u;
    inv.window_end_mono_us = (int64_t)inv.window_seq * 1000000;
    inv.quality = AE_QUALITY_INVALID;
    live.last_window_seq = inv.window_seq;
    ear_obs_manager_on_candidate_evidence(&f.om, cid, &inv, &live);

    EXPECT(f.om.bind[bi].snap.sig_count == n0);
    EXPECT(f.om.bind[bi].last_owned_sig_window_seq == top0);
    ear_obs_manager_diag_t d1;
    ear_obs_manager_get_diag(&f.om, &d1);
    EXPECT(d1.evidence_sig_rejected >= d0.evidence_sig_rejected + 2u);
    fixture_fini(&f);
}

/** 5. Duplicate and out-of-order window_seq rejected for signature ring. */
static void test_s2_dup_and_out_of_order_seq(void)
{
    fixture_t f;
    fixture_init(&f, false);

    publish_shaped(&f, 1u, -40.0f, k_band_a, 120.0f);
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&f.ce, 0u, &c));
    const uint64_t cid = c.candidate_id;
    EXPECT(promote_with_reserve(&f, cid, 1000));

    publish_shaped(&f, 2u, -39.0f, k_band_a, 121.0f);
    ear_obs_manager_feed_from_ce(&f.om, &f.ce);
    const int bi = find_bind_idx(&f, cid);
    EXPECT(bi >= 0);
    const uint8_t n1 = f.om.bind[bi].snap.sig_count;

    ear_obs_manager_diag_t d0;
    ear_obs_manager_get_diag(&f.om, &d0);
    /* Duplicate: feed again without new CE association. */
    ear_obs_manager_feed_from_ce(&f.om, &f.ce);
    EXPECT(f.om.bind[bi].snap.sig_count == n1);
    ear_obs_manager_diag_t d1;
    ear_obs_manager_get_diag(&f.om, &d1);
    EXPECT(d1.evidence_sig_rejected > d0.evidence_sig_rejected);

    /* Out-of-order: offer seq 1 while last owned is 2. */
    ae_signature_t old;
    EXPECT(ce_get_last_evidence_signature(&f.ce, cid, &old));
    old.window_seq = 1u;
    old.window_end_mono_us = 1000000;
    ae_candidate_t live;
    EXPECT(ce_find_candidate(&f.ce, cid, &live));
    live.last_window_seq = 1u;
    live.promotion_gen = f.om.bind[bi].ce_promotion_gen;
    ear_obs_manager_on_candidate_evidence(&f.om, cid, &old, &live);
    EXPECT(f.om.bind[bi].snap.sigs[0].window_seq == 2u);
    fixture_fini(&f);
}

/** 6. Stale promotion_gen cannot update a detached or rebound Observation. */
static void test_s2_stale_promotion_gen_dropped(void)
{
    fixture_t f;
    fixture_init(&f, false);

    publish_shaped(&f, 1u, -40.0f, k_band_a, 120.0f);
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&f.ce, 0u, &c));
    const uint64_t cid = c.candidate_id;
    EXPECT(promote_with_reserve(&f, cid, 1000));
    const int bi = find_bind_idx(&f, cid);
    EXPECT(bi >= 0);
    const uint32_t gen1 = f.om.bind[bi].ce_promotion_gen;
    EXPECT(gen1 != 0u);
    const uint8_t n0 = f.om.bind[bi].snap.sig_count;

    /* Detached (!ce_live): evidence dropped entirely. */
    EXPECT(ce_find_candidate(&f.ce, cid, &c));
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, 2000);
    EXPECT(!f.om.bind[bi].ce_live);

    ear_obs_manager_diag_t d0;
    ear_obs_manager_get_diag(&f.om, &d0);
    ae_signature_t sig;
    memset(&sig, 0, sizeof(sig));
    sig.schema_version = AE_SIGNATURE_SCHEMA_VERSION;
    sig.window_seq = 50u;
    sig.window_end_mono_us = 50000000;
    sig.window_ms = 1000;
    sig.quality = AE_QUALITY_OK;
    sig.rms_dbfs = -35.0f;
    c.last_window_seq = 50u;
    c.promotion_gen = gen1;
    ear_obs_manager_on_candidate_evidence(&f.om, cid, &sig, &c);
    ear_obs_manager_diag_t d1;
    ear_obs_manager_get_diag(&f.om, &d1);
    EXPECT(d1.evidence_dropped == d0.evidence_dropped + 1u);

    /* Rebound association: restore ce_live with a newer gen; stale gen1 dropped. */
    f.om.bind[bi].ce_live = true;
    f.om.bind[bi].ce_promotion_gen = gen1 + 7u;
    ear_obs_manager_diag_t d2;
    ear_obs_manager_get_diag(&f.om, &d2);
    c.promotion_gen = gen1; /* stale */
    c.last_window_seq = 51u;
    sig.window_seq = 51u;
    sig.window_end_mono_us = 51000000;
    ear_obs_manager_on_candidate_evidence(&f.om, cid, &sig, &c);
    ear_obs_manager_diag_t d3;
    ear_obs_manager_get_diag(&f.om, &d3);
    EXPECT(d3.evidence_dropped == d2.evidence_dropped + 1u);
    EXPECT(f.om.bind[bi].snap.sig_count == n0);
    fixture_fini(&f);
}

/** 7. ENTER snapshot seeding from AE history remains unchanged. */
static void test_s2_enter_snapshot_unchanged(void)
{
    fixture_t f;
    fixture_init(&f, false);

    publish_shaped(&f, 1u, -40.0f, k_band_a, 120.0f);
    publish_shaped(&f, 2u, -39.0f, k_band_a, 121.0f);
    publish_shaped(&f, 3u, -38.0f, k_band_a, 122.0f);
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&f.ce, 0u, &c));
    const uint64_t cid = c.candidate_id;

    ae_signature_t hist[EAR_OBS_PROMOTE_SIG_CAP];
    const uint32_t hn = ae_history_copy(&f.ae, hist, EAR_OBS_PROMOTE_SIG_CAP);
    EXPECT(hn >= 1u);

    EXPECT(promote_with_reserve(&f, cid, 5000));
    const int bi = find_bind_idx(&f, cid);
    EXPECT(bi >= 0);
    EXPECT(f.om.bind[bi].snap.sig_count == (uint8_t)hn);
    for (uint32_t i = 0; i < hn; i++) {
        EXPECT(f.om.bind[bi].snap.sigs[i].window_seq == hist[i].window_seq);
        EXPECT(f.om.bind[bi].snap.sigs[i].rms_dbfs == hist[i].rms_dbfs);
    }
    EXPECT(f.om.bind[bi].last_owned_sig_window_seq == hist[0].window_seq);
    fixture_fini(&f);
}

/** Provenance: AE latest must not attach when it is not the candidate update. */
static void test_s2_rejects_unassociated_ae_latest(void)
{
    fixture_t f;
    fixture_init(&f, false);

    publish_shaped(&f, 1u, -40.0f, k_band_a, 120.0f);
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&f.ce, 0u, &c));
    const uint64_t cid = c.candidate_id;
    EXPECT(promote_with_reserve(&f, cid, 1000));
    const int bi = find_bind_idx(&f, cid);
    EXPECT(bi >= 0);
    const uint32_t top0 = f.om.bind[bi].last_owned_sig_window_seq;

    /* Unrelated AE publish that does not rematch this candidate (other shape). */
    publish_shaped(&f, 2u, -40.0f, k_band_b, 400.0f);
    ae_signature_t ae_latest;
    EXPECT(ae_get_latest_signature(&f.ae, &ae_latest));
    EXPECT(ae_latest.window_seq == 2u);

    EXPECT(ce_find_candidate(&f.ce, cid, &c));
    /* Candidate A still at seq 1; forcing AE latest onto it must fail provenance. */
    c.promotion_gen = f.om.bind[bi].ce_promotion_gen;
    ear_obs_manager_diag_t d0;
    ear_obs_manager_get_diag(&f.om, &d0);
    ear_obs_manager_on_candidate_evidence(&f.om, cid, &ae_latest, &c);
    ear_obs_manager_diag_t d1;
    ear_obs_manager_get_diag(&f.om, &d1);
    EXPECT(d1.evidence_sig_rejected > d0.evidence_sig_rejected);
    EXPECT(f.om.bind[bi].last_owned_sig_window_seq == top0);
    fixture_fini(&f);
}

/* --- Stage 3: detached-Observation END watchdog --- */

static bool promote_as002_with_reserve(fixture_t *f, uint64_t cid, int64_t now_us)
{
    ear_obs_rsv_handle_t h;
    if (ear_obs_manager_reserve(&f->om, &h) != EAR_OBS_RSV_OK) {
        return false;
    }
    if (!ear_obs_manager_bind_reservation_candidate(&f->om, h, cid)) {
        ear_obs_manager_release_reservation(&f->om, h);
        return false;
    }
    if (!ear_obs_manager_stamp_reservation_relevance(
            &f->om, h, cid, AS_PROFILE_ID_AS002, 1.0f
        )) {
        ear_obs_manager_release_reservation(&f->om, h);
        return false;
    }
    if (!ce_promote(&f->ce, cid)) {
        ear_obs_manager_release_reservation(&f->om, h);
        return false;
    }
    host_obs_poll(&f->om, now_us);
    return true;
}

/** Matching EXIT arms watchdog; expiry ENDs without RMS hint. */
static void test_s3_detached_ends_after_timeout(void)
{
    fixture_t f;
    fixture_init(&f, false);
    f.om.cfg.detached_end_timeout_ms = 1000u;

    plant(&f.ce, 900u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 900u, 1000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(g_emit.ends == 0u);

    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 900u, &c));
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, 1500);
    const int bi = find_bind_idx(&f, 900u);
    EXPECT(bi >= 0);
    EXPECT(!f.om.bind[bi].ce_live);
    EXPECT(f.om.bind[bi].detached_since_mono_us == 1500);
    EXPECT(g_emit.ends == 0u);

    /* timeout_ms=1000 → 1_000_000 us; expire at since + timeout. */
    host_obs_poll(&f.om, 1500 + 999999);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(g_emit.ends == 0u);

    host_obs_poll(&f.om, 1500 + 1000000);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(g_emit.ends == 1u);
    EXPECT(g_emit.last_end_reason == EAR_OBS_END_EXPLICIT);
    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.detached_watchdog_end == 1u);
    fixture_fini(&f);
}

/**
 * Promotion-gated Obs without any RMS END hint (detector IDLE analogue)
 * still closes via detached watchdog after EXIT.
 */
static void test_s3_idle_rms_path_ends_after_detach_timeout(void)
{
    fixture_t f;
    fixture_init(&f, false);
    f.om.cfg.detached_end_timeout_ms = 500u;

    plant(&f.ce, 901u, AE_CAND_ACTIVE, 3u, -70.0f); /* below RMS ENTER, above CE floor */
    EXPECT(promote_with_reserve(&f, 901u, 1000));
    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 901u, &c));
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, 1000);
    /* No activity_end_hint — only watchdog. timeout_ms=500 → 500_000 us. */
    host_obs_poll(&f.om, 1000 + 499999);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    host_obs_poll(&f.om, 1000 + 500000);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(g_emit.last_end_reason == EAR_OBS_END_EXPLICIT);
    fixture_fini(&f);
}

/** Re-promote / ENTER re-attach cancels the detached timer. */
static void test_s3_reattach_cancels_timer(void)
{
    fixture_t f;
    fixture_init(&f, false);
    f.om.cfg.detached_end_timeout_ms = 1000u;

    plant(&f.ce, 902u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 902u, 1000));
    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 902u, &c));
    const uint32_t gen1 = c.promotion_gen;
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, 1100);
    const int bi = find_bind_idx(&f, 902u);
    EXPECT(bi >= 0);
    EXPECT(f.om.bind[bi].detached_since_mono_us == 1100);

    /* Unadmit-style: restore ACTIVE, clear promotion_entered, re-promote. */
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (f.ce.slots[i].candidate_id == 902u) {
            f.ce.slots[i].state = AE_CAND_ACTIVE;
            f.ce.slots[i].promotion_entered = false;
            f.ce.slots[i].promotion_gen = 0u;
            break;
        }
    }
    EXPECT(promote_with_reserve(&f, 902u, 1200));
    EXPECT(f.om.bind[bi].ce_live);
    EXPECT(f.om.bind[bi].detached_since_mono_us == 0);
    EXPECT(f.om.bind[bi].ce_promotion_gen != gen1);
    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.enter_reattach >= 1u);
    EXPECT(d.detached_watchdog_cancelled >= 1u);

    /* Past original since+timeout: cancelled timer must not END. */
    host_obs_poll(&f.om, 1100 + 1000000);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(g_emit.ends == 0u);
    fixture_fini(&f);
}

/** Two Obs: only the detached binding ends on watchdog. */
static void test_s3_two_obs_only_detached_ends(void)
{
    fixture_t f;
    fixture_init(&f, false);
    f.om.cfg.detached_end_timeout_ms = 1000u;

    plant(&f.ce, 910u, AE_CAND_ACTIVE, 5u, -30.0f);
    plant(&f.ce, 911u, AE_CAND_ACTIVE, 5u, -31.0f);
    EXPECT(promote_with_reserve(&f, 910u, 1000));
    EXPECT(promote_with_reserve(&f, 911u, 2000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);

    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 910u, &c));
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, 2100);
    EXPECT(!ear_obs_manager_binding_ce_live(&f.om, 910u));
    EXPECT(ear_obs_manager_binding_ce_live(&f.om, 911u));

    /* timeout_ms=1000 → expire detached 910 only at 2100 + 1_000_000. */
    host_obs_poll(&f.om, 2100 + 1000000);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(find_bind_idx(&f, 911u) >= 0);
    EXPECT(find_bind_idx(&f, 910u) < 0);
    EXPECT(g_emit.ends == 1u);
    EXPECT(g_emit.last_end_cand == 910u);
    fixture_fini(&f);
}

/** RMS ambient END still ends non-AS002 immediately (regression). */
static void test_s3_ambient_end_still_immediate(void)
{
    fixture_t f;
    fixture_init(&f, false);
    f.om.cfg.detached_end_timeout_ms = 60000u;

    plant(&f.ce, 920u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 920u, 1000));
    ear_obs_manager_on_activity_end_hint(
        &f.om, EAR_OBS_ACT_END_AMBIENT, 1500, 2000, true, -50.0f
    );
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(g_emit.ends == 1u);
    EXPECT(g_emit.last_end_reason == EAR_OBS_END_HINT_ACTIVITY);
    fixture_fini(&f);
}

/** CE EXIT alone does not immediately END. */
static void test_s3_exit_alone_no_immediate_end(void)
{
    fixture_t f;
    fixture_init(&f, false);
    f.om.cfg.detached_end_timeout_ms = 60000u;

    plant(&f.ce, 921u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 921u, 1000));
    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 921u, &c));
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, 1000);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(!ear_obs_manager_binding_ce_live(&f.om, 921u));
    EXPECT(g_emit.ends == 0u);
    fixture_fini(&f);
}

/** AS002: EXIT does not arm watchdog; signature-absence END unchanged. */
static void test_s3_as002_excluded_from_watchdog(void)
{
    fixture_t f;
    fixture_init(&f, false);
    f.om.cfg.detached_end_timeout_ms = 500u;
    f.om.cfg.as002_signature_absence_hold_ms = 2000u;

    plant(&f.ce, 930u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_as002_with_reserve(&f, 930u, 1000));
    const int bi = find_bind_idx(&f, 930u);
    EXPECT(bi >= 0);
    EXPECT(f.om.bind[bi].as002_sig_track);

    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 930u, &c));
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, 1100);
    EXPECT(!f.om.bind[bi].ce_live);
    EXPECT(f.om.bind[bi].detached_since_mono_us == 0);

    host_obs_poll(&f.om, 5000);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(g_emit.ends == 0u);

    /* Signature-absence still ends AS002. */
    ear_obs_manager_on_as002_siren_window(
        &f.om, true, false, 1000u, 6000, true, -40.0f
    );
    ear_obs_manager_on_as002_siren_window(
        &f.om, true, false, 1000u, 7000, true, -40.0f
    );
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(g_emit.last_end_reason == EAR_OBS_END_SIGNATURE);
    fixture_fini(&f);
}

/** Stale promotion_gen EXIT must not arm the watchdog. */
static void test_s3_stale_gen_no_timer(void)
{
    fixture_t f;
    fixture_init(&f, false);
    f.om.cfg.detached_end_timeout_ms = 500u;

    plant(&f.ce, 940u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 940u, 1000));
    const int bi = find_bind_idx(&f, 940u);
    EXPECT(bi >= 0);
    const uint32_t live_gen = f.om.bind[bi].ce_promotion_gen;

    ae_candidate_t stale;
    EXPECT(ce_find_candidate(&f.ce, 940u, &stale));
    stale.promotion_gen = live_gen + 99u;
    ear_obs_manager_on_promotion(&stale, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, 1100);
    EXPECT(f.om.bind[bi].ce_live);
    EXPECT(f.om.bind[bi].detached_since_mono_us == 0);
    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.exit_stale >= 1u);
    EXPECT(d.detached_watchdog_armed == 0u);

    host_obs_poll(&f.om, 10000);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    fixture_fini(&f);
}

/** Reused binding slot does not inherit an earlier detached timestamp. */
static void test_s3_reused_binding_no_inherited_detach(void)
{
    fixture_t f;
    fixture_init(&f, false);
    f.om.cfg.detached_end_timeout_ms = 1000u;

    plant(&f.ce, 950u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 950u, 1000));
    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 950u, &c));
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, 1100);
    const int bi0 = find_bind_idx(&f, 950u);
    EXPECT(bi0 >= 0);
    EXPECT(f.om.bind[bi0].detached_since_mono_us == 1100);

    ear_obs_manager_force_end_all(&f.om, EAR_OBS_END_EXPLICIT, 1200);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);

    plant(&f.ce, 951u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 951u, 2000));
    const int bi1 = find_bind_idx(&f, 951u);
    EXPECT(bi1 >= 0);
    EXPECT(f.om.bind[bi1].ce_live);
    EXPECT(f.om.bind[bi1].detached_since_mono_us == 0);

    /* Even if same slot index as before, no inherited timer. */
    host_obs_poll(&f.om, 5000);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    fixture_fini(&f);
}

/** Timeout boundary: expire exactly at since+timeout; reject clock rewind. */
static void test_s3_timeout_boundary_monotonic(void)
{
    fixture_t f;
    fixture_init(&f, false);
    f.om.cfg.detached_end_timeout_ms = 1000u;

    plant(&f.ce, 960u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 960u, 1000));
    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 960u, &c));
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, 2000000); /* detach at 2_000_000 us */
    const int bi = find_bind_idx(&f, 960u);
    EXPECT(bi >= 0);
    EXPECT(f.om.bind[bi].detached_since_mono_us == 2000000);

    host_obs_poll(&f.om, 2999999);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);

    /* Non-monotonic (rewind): must not END. */
    host_obs_poll(&f.om, 1000000);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);

    host_obs_poll(&f.om, 3000000); /* exactly +1000 ms */
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(g_emit.last_end_reason == EAR_OBS_END_EXPLICIT);
    fixture_fini(&f);
}

/** PERIODIC hints must not refresh or clear detached_since_mono_us. */
static void test_s3_periodic_does_not_touch_detached_timer(void)
{
    fixture_t f;
    fixture_init(&f, false);
    f.om.cfg.detached_end_timeout_ms = 1000u;
    f.om.cfg.enable_poll_periodic = false;

    plant(&f.ce, 970u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 970u, 1000));
    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 970u, &c));
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, 2000000);
    const int bi = find_bind_idx(&f, 970u);
    EXPECT(bi >= 0);
    EXPECT(f.om.bind[bi].detached_since_mono_us == 2000000);
    const int64_t since = f.om.bind[bi].detached_since_mono_us;

    ear_obs_manager_on_activity_periodic_hint(
        &f.om, 2000000, 2500000, true, -40.0f
    );
    EXPECT(g_emit.periodics == 1u);
    EXPECT(f.om.bind[bi].detached_since_mono_us == since);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(g_emit.ends == 0u);

    /* Still expires from original since, not from PERIODIC time. */
    host_obs_poll(&f.om, since + 999999);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    host_obs_poll(&f.om, since + 1000000);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(g_emit.ends == 1u);
    EXPECT(g_emit.last_end_reason == EAR_OBS_END_EXPLICIT);
    fixture_fini(&f);
}

/**
 * engine_stop detaches without END; subsequent poll past detached timeout
 * emits exactly one EAR_OBS_END_EXPLICIT.
 */
static void test_s3_engine_stop_then_watchdog_end(void)
{
    fixture_t f;
    fixture_init(&f, false);
    f.om.cfg.detached_end_timeout_ms = 1000u;

    plant(&f.ce, 980u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 980u, 1000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(g_emit.ends == 0u);

    ce_stop(&f.ce);
    host_obs_poll(&f.om, 2000000); /* drain EXIT from stop */
    ear_obs_manager_on_engine_stop(&f.om, 2000000);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(g_emit.ends == 0u);
    const int bi = find_bind_idx(&f, 980u);
    EXPECT(bi >= 0);
    EXPECT(!f.om.bind[bi].ce_live);
    EXPECT(f.om.bind[bi].detached_since_mono_us == 2000000);

    host_obs_poll(&f.om, 2000000 + 999999);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(g_emit.ends == 0u);

    host_obs_poll(&f.om, 2000000 + 1000000);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(g_emit.ends == 1u);
    EXPECT(g_emit.last_end_reason == EAR_OBS_END_EXPLICIT);
    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.detached_watchdog_end == 1u);

    /* Further polls must not emit a second END. */
    host_obs_poll(&f.om, 2000000 + 2000000);
    EXPECT(g_emit.ends == 1u);
    fixture_fini(&f);
}

/* 24–25. Evidence owned after CE clear; no dangling CE pointers in snap */
static void test_evidence_owned(void)
{
    fixture_t f;
    fixture_init(&f, false);
    publish_tone(&f.ae, 1u, -40.0f);
    publish_tone(&f.ae, 2u, -35.0f);
    publish_tone(&f.ae, 3u, -33.0f);

    /* Activate via AE→CE */
    ae_candidate_t c;
    bool found = false;
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (f.ce.slots[i].in_use && f.ce.slots[i].state == AE_CAND_ACTIVE) {
            c = f.ce.slots[i];
            found = true;
            break;
        }
    }
    if (!found) {
        /* Force plant if AE path didn't activate yet */
        plant(&f.ce, 200u, AE_CAND_ACTIVE, 5u, -33.0f);
        EXPECT(ce_find_candidate(&f.ce, 200u, &c));
    } else {
        EXPECT(ce_find_candidate(&f.ce, c.candidate_id, &c));
    }

    const uint64_t cid = c.candidate_id;
    EXPECT(promote_with_reserve(&f, cid, 5000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);

    ear_obs_promote_snapshot_t snap_copy;
    memset(&snap_copy, 0, sizeof(snap_copy));
    for (uint8_t i = 0; i < 2; i++) {
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == cid) {
            snap_copy = f.om.bind[i].snap;
        }
    }
    EXPECT(snap_copy.candidate_id == cid);

    /* Clear CE slot */
    EXPECT(ce_reject(&f.ce, cid));
    host_obs_poll(&f.om, 6000);

    /* Snapshot remains in binding after EXIT */
    bool ok = false;
    for (uint8_t i = 0; i < 2; i++) {
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == cid) {
            EXPECT(f.om.bind[i].snap.candidate_id == cid);
            EXPECT(f.om.bind[i].snap.ema_rms_dbfs == snap_copy.ema_rms_dbfs);
            /* sigs are by-value copies, not CE pointers */
            ok = true;
        }
    }
    EXPECT(ok);
    fixture_fini(&f);
}

/* Policy: reserve-full prevents promote (approved candidates only) */
static void test_policy_reserve_gates(void)
{
    fixture_t f;
    fixture_init(&f, true);
    plant(&f.ce, 300u, AE_CAND_ACTIVE, 5u, -30.0f);
    plant(&f.ce, 301u, AE_CAND_ACTIVE, 4u, -31.0f);
    plant(&f.ce, 302u, AE_CAND_ACTIVE, 3u, -32.0f);
    plant(&f.ce, 303u, AE_CAND_ACTIVE, 2u, -33.0f);
    mark_as001_relevant(&f.ce, 300u);
    mark_as001_relevant(&f.ce, 301u);
    mark_as001_relevant(&f.ce, 302u);
    mark_as001_relevant(&f.ce, 303u);

    for (int i = 0; i < 6; i++) {
        ear_promotion_policy_poll(&f.pol, (int64_t)(i + 1) * 1000);
        host_obs_poll(&f.om, (int64_t)(i + 1) * 1000);
    }
    EXPECT(ear_obs_manager_n_open(&f.om) <= 2u);
    EXPECT(ear_obs_manager_n_held(&f.om) + ear_obs_manager_n_pending(&f.om) <= 2u);
    EXPECT(g_emit.starts >= 1u);
    fixture_fini(&f);
}

/* ENTER without reservation must not allocate */
static void test_orphan_enter(void)
{
    fixture_t f;
    fixture_init(&f, false);
    ae_candidate_t c;
    memset(&c, 0, sizeof(c));
    c.candidate_id = 999u;
    c.state = AE_CAND_PROMOTED;
    c.in_use = true;
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_ENTER, &f.om);
    host_obs_poll(&f.om, 1000);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(g_emit.starts == 0u);
    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.enter_no_rsv >= 1u);
    fixture_fini(&f);
}

/**
 * Capture the first CE EXIT payload (including promotion_gen) before forwarding
 * to OM. Used to prove gen originates in CE emit_exit, not a hand-built value.
 */
static ae_candidate_t g_captured_exit;
static uint32_t g_captured_exit_n;

static void om_promo_capture_exit(
    const ae_candidate_t *c,
    ce_promotion_event_t ev,
    void *ctx
)
{
    if (c != NULL && ev == CE_PROMOTION_EXIT) {
        if (g_captured_exit_n == 0u) {
            g_captured_exit = *c;
        }
        g_captured_exit_n++;
    }
    ear_obs_manager_on_promotion(c, ev, ctx);
}

/**
 * End-to-end: CE emit_exit → callback cand_copy → OM evq → handle_exit.
 * Delayed redelivery of the *real* first EXIT payload (G1) must not affect a
 * newer Observation that reused the same candidate_id (G2).
 */
static void test_ce_exit_payload_gen_propagates_stale_safe(void)
{
    fixture_t f;
    fixture_init(&f, false);
    memset(&g_captured_exit, 0, sizeof(g_captured_exit));
    g_captured_exit_n = 0u;
    ce_set_promotion_cb(&f.ce, om_promo_capture_exit, &f.om);

    plant(&f.ce, 500u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 500u, 1000));

    ae_candidate_t live;
    EXPECT(ce_find_candidate(&f.ce, 500u, &live));
    const uint32_t gen1 = live.promotion_gen;
    EXPECT(gen1 != 0u);
    char old_obs[SP_OBS_EPISODE_ID_LEN];
    memcpy(old_obs, g_emit.last_obs_id, sizeof(old_obs));

    uint8_t slot = 255u;
    for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == 500u) {
            slot = i;
            EXPECT(f.om.bind[i].ce_promotion_gen == gen1);
            break;
        }
    }
    EXPECT(slot != 255u);

    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 2000, 2100, true, -50.0f);
    EXPECT(g_emit.ends == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);

    /* Real CE EXIT: capture payload gen at callback time, then drain. */
    EXPECT(ce_reject(&f.ce, 500u));
    host_obs_poll(&f.om, 2200);
    EXPECT(g_emit.ends == 1u);
    EXPECT(g_captured_exit_n >= 1u);
    EXPECT(g_captured_exit.candidate_id == 500u);
    EXPECT(g_captured_exit.promotion_gen == gen1);

    /* Reuse the same candidate_id after slot clear → new engine-monotonic gen. */
    plant(&f.ce, 500u, AE_CAND_ACTIVE, 5u, -28.0f);
    EXPECT(promote_with_reserve(&f, 500u, 3000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    char new_obs[SP_OBS_EPISODE_ID_LEN];
    memcpy(new_obs, g_emit.last_obs_id, sizeof(new_obs));
    EXPECT(strncmp(old_obs, new_obs, SP_OBS_EPISODE_ID_LEN) != 0);

    EXPECT(ce_find_candidate(&f.ce, 500u, &live));
    const uint32_t gen2 = live.promotion_gen;
    EXPECT(gen2 != 0u);
    EXPECT(gen2 != gen1);
    EXPECT(f.om.bind[slot].in_use);
    EXPECT(f.om.bind[slot].candidate_id == 500u);
    EXPECT(f.om.bind[slot].ce_promotion_gen == gen2);
    EXPECT(ear_obs_manager_binding_ce_live(&f.om, 500u));
    /* Captured EXIT still carries G1 — not rewritten by the newer candidate. */
    EXPECT(g_captured_exit.promotion_gen == gen1);

    ear_obs_manager_diag_t d0;
    ear_obs_manager_get_diag(&f.om, &d0);
    ear_obs_manager_on_promotion(&g_captured_exit, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, 3100);
    ear_obs_manager_diag_t d1;
    ear_obs_manager_get_diag(&f.om, &d1);
    EXPECT(d1.exit_stale == d0.exit_stale + 1u);
    EXPECT(ear_obs_manager_binding_ce_live(&f.om, 500u));
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(g_emit.ends == 1u);
    EXPECT(strncmp(g_emit.last_obs_id, new_obs, SP_OBS_EPISODE_ID_LEN) == 0);

    /* Valid EXIT for G2 (live CE reject): detach-only. */
    EXPECT(ce_reject(&f.ce, 500u));
    host_obs_poll(&f.om, 3200);
    EXPECT(!ear_obs_manager_binding_ce_live(&f.om, 500u));
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(g_emit.ends == 1u);

    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 3300, 3400, true, -52.0f);
    EXPECT(g_emit.ends == 2u);
    EXPECT(strncmp(g_emit.last_end_obs_id, new_obs, SP_OBS_EPISODE_ID_LEN) == 0);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);
    fixture_fini(&f);
}

/**
 * Production OM callback path: zero-gen EXIT must not detach a gen-tagged
 * live binding (no candidate_id-only fallback). No END / registry release.
 */
static void test_zero_gen_exit_rejected_on_live_binding(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 540u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 540u, 1000));
    EXPECT(ear_obs_manager_binding_ce_live(&f.om, 540u));
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 1u);

    uint32_t bound_gen = 0u;
    for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == 540u) {
            bound_gen = f.om.bind[i].ce_promotion_gen;
            break;
        }
    }
    EXPECT(bound_gen != 0u);

    ear_obs_manager_diag_t d0;
    ear_obs_manager_get_diag(&f.om, &d0);
    const uint32_t ends0 = g_emit.ends;
    const uint32_t held0 = ear_obs_manager_n_held(&f.om);

    ae_candidate_t zero;
    memset(&zero, 0, sizeof(zero));
    zero.candidate_id = 540u;
    /* promotion_gen == 0: public production callback API. */
    ear_obs_manager_on_promotion(&zero, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, 1500);

    ear_obs_manager_diag_t d1;
    ear_obs_manager_get_diag(&f.om, &d1);
    EXPECT(d1.exit_stale == d0.exit_stale + 1u);
    EXPECT(d1.exit_detach == d0.exit_detach);
    EXPECT(ear_obs_manager_binding_ce_live(&f.om, 540u));
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 1u);
    EXPECT(g_emit.ends == ends0);
    EXPECT(ear_obs_manager_n_held(&f.om) == held0);

    /* Valid matching EXIT still detach-only; activity END remains terminator. */
    EXPECT(ce_reject(&f.ce, 540u));
    host_obs_poll(&f.om, 1600);
    EXPECT(!ear_obs_manager_binding_ce_live(&f.om, 540u));
    EXPECT(g_emit.ends == ends0);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);

    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 1700, 1800, true, -50.0f);
    EXPECT(g_emit.ends == ends0 + 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);
    fixture_fini(&f);
}

/* END metadata is snapshotted before the binding slot is cleared. */
static void test_end_metadata_snapshot(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 510u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 510u, 12345));
    char obs_id[SP_OBS_EPISODE_ID_LEN];
    memcpy(obs_id, g_emit.last_obs_id, sizeof(obs_id));
    int64_t start_mono = 0;
    for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == 510u) {
            start_mono = f.om.bind[i].start_mono_us;
            break;
        }
    }
    EXPECT(start_mono > 0);

    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 20000, 21000, true, -41.5f);
    EXPECT(g_emit.ends == 1u);
    EXPECT(g_emit.last_end_cand == 510u);
    EXPECT(strncmp(g_emit.last_end_obs_id, obs_id, SP_OBS_EPISODE_ID_LEN) == 0);
    EXPECT(g_emit.last_end_binding_start_mono_us == start_mono);
    EXPECT(g_emit.last_end_start == 20000);
    EXPECT(g_emit.last_end_end == 21000);
    EXPECT(g_emit.last_end_has_lvl);
    EXPECT(g_emit.last_end_lvl == -41.5f);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    fixture_fini(&f);
}

/* Publication failure must not leave binding / registry capacity stuck. */
static void test_end_publish_fail_still_releases(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 520u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 520u, 1000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 1u);

    g_emit.fail_end = true;
    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 2000, 2100, true, -50.0f);
    g_emit.fail_end = false;

    EXPECT(g_emit.ends == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);
    EXPECT(ear_obs_manager_n_held(&f.om) == 0u);

    plant(&f.ce, 521u, AE_CAND_ACTIVE, 4u, -29.0f);
    EXPECT(promote_with_reserve(&f, 521u, 3000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 1u);
    fixture_fini(&f);
}

/**
 * Re-entrant activity END from emit_end: terminal clear prevents a second END
 * and a second registry release.
 */
static void test_end_reentrant_exactly_once(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 530u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 530u, 1000));
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 1u);

    g_emit.reenter_om = &f.om;
    g_emit.reenter_end = true;
    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 2000, 2100, true, -50.0f);
    EXPECT(!g_emit.reenter_end);
    EXPECT(g_emit.ends == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);

    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.end_done == 1u);

    /* Repeated END after cleanup remains harmless. */
    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 2200, 2300, true, -50.0f);
    EXPECT(g_emit.ends == 1u);
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.end_done == 1u);
    fixture_fini(&f);
}

/* ---- P2 product-relevance promotion gate ---- */

static bool find_binding_like(fixture_t *f, uint64_t cid)
{
    for (uint8_t i = 0; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (f->om.bind[i].in_use && f->om.bind[i].candidate_id == cid) {
            return true;
        }
    }
    return false;
}

static void test_p2_ambient_active_no_obs(void)
{
    fixture_t f;
    fixture_init(&f, true);
    plant(&f.ce, 700u, AE_CAND_ACTIVE, 20u, -10.0f); /* loud ambient */
    for (int i = 0; i < 4; i++) {
        ear_promotion_policy_poll(&f.pol, (int64_t)(i + 1) * 1000);
        host_obs_poll(&f.om, (int64_t)(i + 1) * 1000);
    }
    EXPECT(g_emit.starts == 0u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(ear_obs_manager_n_pending(&f.om) == 0u);
    EXPECT(ear_obs_manager_n_held(&f.om) == 0u);
    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 700u, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    EXPECT(!c.promotion_entered);
    ear_promotion_policy_diag_t pd;
    ear_promotion_policy_get_diag(&f.pol, &pd);
    EXPECT(pd.hold_no_class + pd.hold_irrelevant >= 1u);
    EXPECT(pd.promote_ok == 0u);
    EXPECT(pd.reserve_ok == 0u);
    fixture_fini(&f);
}

static void test_p2_as001_can_promote(void)
{
    fixture_t f;
    fixture_init(&f, true);
    plant(&f.ce, 710u, AE_CAND_ACTIVE, 5u, -30.0f);
    mark_as001_relevant(&f.ce, 710u);
    ear_promotion_policy_poll(&f.pol, 1000);
    host_obs_poll(&f.om, 1000);
    EXPECT(g_emit.starts == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(g_emit.last_start_cand == 710u);
    for (uint8_t i = 0; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == 710u) {
            EXPECT(f.om.bind[i].snap.product_relevance_approved);
            EXPECT(f.om.bind[i].snap.relevance_profile_id == AS_PROFILE_ID_AS001);
            EXPECT(f.om.bind[i].cls_wire.present);
            EXPECT(f.om.bind[i].cls_wire.n_nodes == 1u);
            EXPECT(strcmp(f.om.bind[i].cls_wire.nodes[0].id, EAR_CLS_AS001_NODE_AIRCRAFT)
                   == 0);
        }
    }
    EXPECT(g_emit.last_start_assessment_present);
    EXPECT(strcmp(g_emit.last_start_assessment_path, "AIRCRAFT") == 0);

    /* First PERIODIC deepens to AIRCRAFT > Helicopter. */
    ear_obs_manager_on_activity_periodic_hint(&f.om, 1000, 6000, true, -30.0f);
    EXPECT(g_emit.periodics == 1u);
    EXPECT(g_emit.last_periodic_assessment_present);
    EXPECT(strcmp(g_emit.last_periodic_assessment_path, "AIRCRAFT/Helicopter") == 0);
    for (uint8_t i = 0; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == 710u) {
            EXPECT(f.om.bind[i].cls_wire.n_nodes == 2u);
            EXPECT(strcmp(f.om.bind[i].cls_wire.nodes[1].id, EAR_CLS_AS001_NODE_HELICOPTER)
                   == 0);
        }
    }

    ear_promotion_policy_diag_t pd;
    ear_promotion_policy_get_diag(&f.pol, &pd);
    EXPECT(pd.promote_ok >= 1u);
    EXPECT(pd.last_decision == EAR_PROMO_DECISION_PROMOTE);
    EXPECT(pd.last_profile_id == AS_PROFILE_ID_AS001);
    fixture_fini(&f);
}

static void test_p2_loud_unmatched_not_promoted(void)
{
    fixture_t f;
    fixture_init(&f, true);
    plant(&f.ce, 720u, AE_CAND_ACTIVE, 99u, -5.0f);
    /* Explicit non-match annotation (valid but not AS-001). */
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (f.ce.slots[i].in_use && f.ce.slots[i].candidate_id == 720u) {
            ce_classification_t *cls = &f.ce.classifications[i];
            memset(cls, 0, sizeof(*cls));
            cls->candidate_id = 720u;
            cls->valid = true;
            cls->matched = false;
            cls->profile_id = AS_PROFILE_ID_NONE;
            cls->acoustic_class = AS_CLASS_UNSPECIFIED;
            cls->distance = 99.0f;
            break;
        }
    }
    ear_promotion_policy_poll(&f.pol, 1000);
    host_obs_poll(&f.om, 1000);
    EXPECT(g_emit.starts == 0u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(ear_obs_manager_n_pending(&f.om) == 0u);
    EXPECT(ear_obs_manager_n_held(&f.om) == 0u);
    ear_promotion_policy_diag_t pd;
    ear_promotion_policy_get_diag(&f.pol, &pd);
    EXPECT(pd.hold_irrelevant >= 1u);
    EXPECT(pd.promote_ok == 0u);
    fixture_fini(&f);
}

static void test_p2_held_irrelevant_consumes_no_capacity(void)
{
    fixture_t f;
    fixture_init(&f, true);
    plant(&f.ce, 730u, AE_CAND_ACTIVE, 8u, -15.0f);
    plant(&f.ce, 731u, AE_CAND_ACTIVE, 7u, -16.0f);
    /* No AS-001 stamps. */
    for (int i = 0; i < 3; i++) {
        ear_promotion_policy_poll(&f.pol, (int64_t)(i + 1) * 1000);
        host_obs_poll(&f.om, (int64_t)(i + 1) * 1000);
    }
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(ear_obs_manager_n_pending(&f.om) == 0u);
    EXPECT(ear_obs_manager_n_held(&f.om) == 0u);
    ear_obs_rsv_handle_t h;
    EXPECT(ear_obs_manager_reserve(&f.om, &h) == EAR_OBS_RSV_OK);
    ear_obs_manager_release_reservation(&f.om, h);
    fixture_fini(&f);
}

static void test_p2_pending_entries_are_approved(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 740u, AE_CAND_ACTIVE, 5u, -30.0f);
    plant(&f.ce, 741u, AE_CAND_ACTIVE, 4u, -31.0f);
    plant(&f.ce, 742u, AE_CAND_ACTIVE, 3u, -32.0f);
    EXPECT(promote_with_reserve(&f, 740u, 1000));
    EXPECT(promote_with_reserve(&f, 741u, 2000));
    EXPECT(promote_with_reserve(&f, 742u, 3000));
    EXPECT(ear_obs_manager_n_pending(&f.om) >= 1u);
    for (uint8_t i = 0; i < f.om.pending_count; i++) {
        const uint8_t slot = f.om.pending_order[i];
        EXPECT(f.om.pending[slot].in_use);
        EXPECT(f.om.pending[slot].snap.product_relevance_approved);
        EXPECT(f.om.pending[slot].snap.relevance_profile_id == AS_PROFILE_ID_AS001);
    }
    fixture_fini(&f);
}

static void test_p2_pending_drain_rejects_unapproved(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 750u, AE_CAND_ACTIVE, 5u, -30.0f);
    plant(&f.ce, 751u, AE_CAND_ACTIVE, 4u, -31.0f);
    plant(&f.ce, 752u, AE_CAND_ACTIVE, 6u, -28.0f);
    EXPECT(promote_with_reserve(&f, 750u, 1000));
    EXPECT(promote_with_reserve(&f, 751u, 2000));
    EXPECT(promote_with_reserve(&f, 752u, 3000));
    EXPECT(ear_obs_manager_n_pending(&f.om) == 1u);
    EXPECT(f.om.pending[f.om.pending_order[0]].candidate_id == 752u);

    /* Corrupt stamp: drain must cancel, not invent approval. */
    f.om.pending[f.om.pending_order[0]].snap.product_relevance_approved = false;

    const uint32_t starts_before = g_emit.starts;
    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 4000, 4100, true, -55.0f);
    EXPECT(ear_obs_manager_n_pending(&f.om) == 0u);
    EXPECT(!find_binding_like(&f, 752u));
    EXPECT(g_emit.starts == starts_before); /* drain must not START 752 */
    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.relevance_reject >= 1u);
    fixture_fini(&f);
}

static void test_p2_same_signature_one_observation(void)
{
    fixture_t f;
    fixture_init(&f, true);
    /* Ranking would prefer 761 then 760 — same AS-001 signature must not dual-open. */
    plant(&f.ce, 760u, AE_CAND_ACTIVE, 5u, -30.0f);
    plant(&f.ce, 761u, AE_CAND_ACTIVE, 8u, -25.0f);
    plant(&f.ce, 762u, AE_CAND_ACTIVE, 5u, -35.0f);
    mark_as001_relevant(&f.ce, 760u);
    mark_as001_relevant(&f.ce, 761u);
    mark_as001_relevant(&f.ce, 762u);

    for (int i = 0; i < 6; i++) {
        ear_promotion_policy_poll(&f.pol, (int64_t)(i + 1) * 1000);
        host_obs_poll(&f.om, (int64_t)(i + 1) * 1000);
    }
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(find_binding_like(&f, 761u));
    EXPECT(!find_binding_like(&f, 760u));
    EXPECT(!find_binding_like(&f, 762u));
    EXPECT(ear_obs_manager_n_pending(&f.om) == 0u);
    EXPECT(g_emit.starts == 1u);
    ear_promotion_policy_diag_t pd;
    ear_promotion_policy_get_diag(&f.pol, &pd);
    EXPECT(pd.hold_signature_open >= 1u);
    EXPECT(pd.last_decision == EAR_PROMO_DECISION_HOLD_SIGNATURE_OPEN
           || pd.hold_signature_open >= 1u);
    fixture_fini(&f);
}

static void test_p2_no_repeat_while_promoted(void)
{
    fixture_t f;
    fixture_init(&f, true);
    plant(&f.ce, 770u, AE_CAND_ACTIVE, 5u, -30.0f);
    mark_as001_relevant(&f.ce, 770u);
    ear_promotion_policy_poll(&f.pol, 1000);
    host_obs_poll(&f.om, 1000);
    EXPECT(g_emit.starts == 1u);
    const uint32_t starts = g_emit.starts;
    ear_promotion_policy_diag_t pd0;
    ear_promotion_policy_get_diag(&f.pol, &pd0);
    const uint32_t promote_ok0 = pd0.promote_ok;
    for (int i = 0; i < 4; i++) {
        ear_promotion_policy_poll(&f.pol, (int64_t)(i + 2) * 1000);
        host_obs_poll(&f.om, (int64_t)(i + 2) * 1000);
    }
    EXPECT(g_emit.starts == starts);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    ear_promotion_policy_diag_t pd1;
    ear_promotion_policy_get_diag(&f.pol, &pd1);
    EXPECT(pd1.promote_ok == promote_ok0);
    fixture_fini(&f);
}

/**
 * Controlled path (reserve→ce_promote, no Policy stamp): OM rejects ENTER and
 * CE unadmits — candidate returns to ACTIVE, not stuck PROMOTED.
 */
static void test_p2_commit_rejects_unapproved_snapshot(void)
{
    fixture_t f;
    fixture_init(&f, true);
    plant(&f.ce, 780u, AE_CAND_ACTIVE, 5u, -30.0f);
    /* Do not mark AS-001 / do not stamp reservation. */

    ae_candidate_t before;
    EXPECT(ce_find_candidate(&f.ce, 780u, &before));
    EXPECT(before.state == AE_CAND_ACTIVE);
    EXPECT(!before.promotion_entered);

    ear_obs_rsv_handle_t h;
    EXPECT(ear_obs_manager_reserve(&f.om, &h) == EAR_OBS_RSV_OK);
    EXPECT(ear_obs_manager_bind_reservation_candidate(&f.om, h, 780u));
    EXPECT(ce_promote(&f.ce, 780u));

    ae_candidate_t mid;
    EXPECT(ce_find_candidate(&f.ce, 780u, &mid));
    EXPECT(mid.state == AE_CAND_PROMOTED);
    EXPECT(mid.promotion_entered);
    const uint32_t gen = mid.promotion_gen;
    EXPECT(gen != 0u);

    host_obs_poll(&f.om, 1000);
    EXPECT(g_emit.starts == 0u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(ear_obs_manager_n_pending(&f.om) == 0u);
    EXPECT(ear_obs_manager_n_held(&f.om) == 0u);
    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.relevance_reject >= 1u);
    EXPECT(d.promote_unadmit >= 1u);

    ae_candidate_t after;
    EXPECT(ce_find_candidate(&f.ce, 780u, &after));
    EXPECT(after.state == AE_CAND_ACTIVE);
    EXPECT(!after.promotion_entered);
    host_obs_poll(&f.om, 1500); /* drain EXIT from unadmit */

    ear_promotion_policy_diag_t pd0;
    ear_promotion_policy_get_diag(&f.pol, &pd0);
    const uint32_t promote_ok0 = pd0.promote_ok;
    for (int i = 0; i < 3; i++) {
        ear_promotion_policy_poll(&f.pol, (int64_t)(2000 + i * 1000));
        host_obs_poll(&f.om, (int64_t)(2000 + i * 1000));
    }
    ear_promotion_policy_diag_t pd1;
    ear_promotion_policy_get_diag(&f.pol, &pd1);
    EXPECT(pd1.promote_ok == promote_ok0);
    EXPECT(g_emit.starts == 0u);

    /* Duplicate unadmit with same gen is harmless. */
    EXPECT(!ce_unadmit_promotion(&f.ce, 780u, gen));
    fixture_fini(&f);
}

/**
 * Policy approval ticket survives classification clearing before ENTER.
 * ENTER uses the reservation stamp, not live reconstruction.
 */
static void test_p2_classification_cleared_before_enter(void)
{
    fixture_t f;
    fixture_init(&f, true);
    plant(&f.ce, 790u, AE_CAND_ACTIVE, 5u, -30.0f);
    mark_as001_relevant(&f.ce, 790u);

    ce_classification_t cls0;
    EXPECT(ce_get_classification(&f.ce, 790u, &cls0));
    EXPECT(cls0.matched);
    EXPECT(cls0.profile_id == AS_PROFILE_ID_AS001);

    ear_promotion_policy_poll(&f.pol, 1000);
    ear_promotion_policy_diag_t pd;
    ear_promotion_policy_get_diag(&f.pol, &pd);
    EXPECT(pd.promote_ok >= 1u);
    EXPECT(pd.last_decision == EAR_PROMO_DECISION_PROMOTE);
    EXPECT(pd.last_candidate_id == 790u);

    ae_candidate_t mid;
    EXPECT(ce_find_candidate(&f.ce, 790u, &mid));
    EXPECT(mid.state == AE_CAND_PROMOTED);
    EXPECT(mid.promotion_entered);
    EXPECT(ear_obs_manager_n_held(&f.om) == 1u);
    EXPECT(g_emit.starts == 0u);

    clear_classification(&f.ce, 790u);
    EXPECT(!ce_get_classification(&f.ce, 790u, &cls0));

    host_obs_poll(&f.om, 2000);
    EXPECT(g_emit.starts == 1u);
    EXPECT(g_emit.last_start_cand == 790u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(ear_obs_manager_n_held(&f.om) == 0u);
    for (uint8_t i = 0; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == 790u) {
            EXPECT(f.om.bind[i].snap.product_relevance_approved);
            EXPECT(f.om.bind[i].snap.relevance_ticket != 0u);
            EXPECT(f.om.bind[i].snap.relevance_profile_id == AS_PROFILE_ID_AS001);
        }
    }
    fixture_fini(&f);
}

/**
 * AS-001 annotation may survive GAP, but gap_events freshness blocks Policy
 * even if the candidate is forced back to ACTIVE without a fresh match.
 */
static void test_p2_stale_as001_across_gap(void)
{
    fixture_t f;
    fixture_init(&f, true);
    plant(&f.ce, 800u, AE_CAND_ACTIVE, 5u, -30.0f);
    mark_as001_relevant(&f.ce, 800u);

    ce_classification_t cls;
    EXPECT(ce_get_classification(&f.ce, 800u, &cls));
    EXPECT(cls.matched);
    EXPECT(cls.gap_events_at_match == 0u);

    publish_gap(&f.ae, 1u, -40.0f);

    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 800u, &c));
    EXPECT(c.state == AE_CAND_COASTING);
    EXPECT(c.gap_events >= 1u);
    EXPECT(ce_get_classification(&f.ce, 800u, &cls));
    EXPECT(cls.valid);
    EXPECT(cls.matched);
    EXPECT(cls.gap_events_at_match != c.gap_events);

    /* Force ACTIVE without refreshing classification (rematch-without-match). */
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (f.ce.slots[i].in_use && f.ce.slots[i].candidate_id == 800u) {
            f.ce.slots[i].state = AE_CAND_ACTIVE;
            f.ce.slots[i].coast_windows = 0u;
            break;
        }
    }
    EXPECT(ce_find_candidate(&f.ce, 800u, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    EXPECT(!ear_promotion_candidate_product_relevant(&f.ce, 800u, NULL, NULL));

    ear_promotion_policy_diag_t pd0;
    ear_promotion_policy_get_diag(&f.pol, &pd0);
    const uint32_t promote_ok0 = pd0.promote_ok;
    ear_promotion_policy_poll(&f.pol, 1000);
    host_obs_poll(&f.om, 1000);
    ear_promotion_policy_diag_t pd1;
    ear_promotion_policy_get_diag(&f.pol, &pd1);
    EXPECT(pd1.promote_ok == promote_ok0);
    EXPECT(g_emit.starts == 0u);
    fixture_fini(&f);
}

/**
 * Test/helper path: promote_with_reserve can START an Observation while Policy
 * promote_ok stays zero (not production Policy wiring). Requires explicit stamp.
 */
static void test_p2_promote_with_reserve_bypasses_policy(void)
{
    fixture_t f;
    fixture_init(&f, true);
    plant(&f.ce, 810u, AE_CAND_ACTIVE, 5u, -30.0f);

    ear_promotion_policy_diag_t pd0;
    ear_promotion_policy_get_diag(&f.pol, &pd0);
    EXPECT(pd0.promote_ok == 0u);

    EXPECT(promote_with_reserve(&f, 810u, 1000));
    EXPECT(g_emit.starts == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);

    ear_promotion_policy_diag_t pd1;
    ear_promotion_policy_get_diag(&f.pol, &pd1);
    EXPECT(pd1.promote_ok == 0u);
    fixture_fini(&f);
}

/** ENTER dropped when OM event queue is full → unadmit + release HELD. */
static void test_p2_enter_queue_full_recovers(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 820u, AE_CAND_ACTIVE, 5u, -30.0f);

    ae_candidate_t filler;
    memset(&filler, 0, sizeof(filler));
    filler.candidate_id = 1u;
    filler.promotion_gen = 1u;
    for (uint8_t i = 0; i < EAR_OBS_MGR_EVENT_Q; i++) {
        filler.candidate_id = (uint64_t)(900u + i);
        ear_obs_manager_on_promotion(&filler, CE_PROMOTION_EXIT, &f.om);
    }
    ear_obs_manager_diag_t d0;
    ear_obs_manager_get_diag(&f.om, &d0);
    EXPECT(d0.evq_dropped == 0u);

    mark_as001_relevant(&f.ce, 820u);
    ear_obs_rsv_handle_t h;
    EXPECT(ear_obs_manager_reserve(&f.om, &h) == EAR_OBS_RSV_OK);
    EXPECT(ear_obs_manager_bind_reservation_candidate(&f.om, h, 820u));
    EXPECT(ear_obs_manager_stamp_reservation_relevance(
        &f.om, h, 820u, AS_PROFILE_ID_AS001, 1.0f
    ));
    EXPECT(ce_promote(&f.ce, 820u));

    ear_obs_manager_diag_t d1;
    ear_obs_manager_get_diag(&f.om, &d1);
    EXPECT(d1.evq_dropped >= 1u);
    EXPECT(d1.promote_unadmit >= 1u);
    EXPECT(ear_obs_manager_n_held(&f.om) == 0u);
    EXPECT(g_emit.starts == 0u);

    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 820u, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    EXPECT(!c.promotion_entered);
    fixture_fini(&f);
}

/** Stale unadmit gen cannot clear a newer promotion on the same candidate_id. */
static void test_p2_unadmit_gen_protects_newer(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 830u, AE_CAND_ACTIVE, 5u, -30.0f);
    EXPECT(promote_with_reserve(&f, 830u, 1000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);

    ae_candidate_t c;
    EXPECT(ce_find_candidate(&f.ce, 830u, &c));
    const uint32_t gen1 = c.promotion_gen;
    EXPECT(gen1 != 0u);

    ear_obs_manager_on_activity_end_hint(&f.om, EAR_OBS_ACT_END_AMBIENT, 2000, 2100, true, -50.0f);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(ce_unadmit_promotion(&f.ce, 830u, gen1));
    host_obs_poll(&f.om, 2200);

    EXPECT(ce_find_candidate(&f.ce, 830u, &c));
    EXPECT(!c.promotion_entered);
    EXPECT(c.state == AE_CAND_ACTIVE);

    EXPECT(promote_with_reserve(&f, 830u, 3000));
    EXPECT(ce_find_candidate(&f.ce, 830u, &c));
    const uint32_t gen2 = c.promotion_gen;
    EXPECT(gen2 != 0u);
    EXPECT(gen2 != gen1);
    EXPECT(c.promotion_entered);

    EXPECT(!ce_unadmit_promotion(&f.ce, 830u, gen1));
    EXPECT(ce_find_candidate(&f.ce, 830u, &c));
    EXPECT(c.promotion_entered);
    EXPECT(c.promotion_gen == gen2);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    fixture_fini(&f);
}

/**
 * Package 2 — CE → OM end-to-end AS-001 lifecycle (real SE/CE/Policy/OM).
 *
 * Synthetic heli/broadband signatures match test_sig_engine_ce_primary.
 * No planted CE slots, no manual relevance stamps, no fabricated ENTER/EXIT.
 * Production order: SE listener before CE; Policy poll before OM poll; then
 * candidate-associated evidence feed.
 *
 * E2E_REQUIRE records failure and jumps to cleanup — never index bind[-1] or
 * continue after a missing Observation / binding prerequisite.
 */
#define E2E_REQUIRE(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
            goto e2e_cleanup;                                                  \
        }                                                                      \
    } while (0)

static void fill_heli_sig(ae_signature_t *s, uint32_t seq, float rms_dbfs)
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

static void fill_broadband_sig(ae_signature_t *s, uint32_t seq, float rms_dbfs)
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

static void fill_gap_sig(ae_signature_t *s, uint32_t seq, float rms_dbfs)
{
    fill_heli_sig(s, seq, rms_dbfs);
    s->quality = AE_QUALITY_GAP;
}

static bool binding_owned_has_gap(const ear_obs_manager_t *om, uint8_t bi)
{
    if (om == NULL || bi >= EAR_OBS_MGR_BINDING_CAP || !om->bind[bi].in_use) {
        return false;
    }
    for (uint8_t i = 0u; i < om->bind[bi].snap.sig_count
         && i < EAR_OBS_PROMOTE_SIG_CAP;
         i++) {
        if (om->bind[bi].snap.sigs[i].quality == AE_QUALITY_GAP) {
            return true;
        }
    }
    return false;
}

static bool e2e_drive(
    fixture_t *f,
    const ae_signature_t *sig,
    int64_t now_mono_us
)
{
    if (f == NULL || sig == NULL) {
        return false;
    }
    if (!ae_publish_signature(&f->ae, sig)) {
        return false;
    }
    ear_promotion_policy_poll(&f->pol, now_mono_us);
    host_obs_poll(&f->om, now_mono_us);
    /* Observable CE → OM live-evidence path (must not be a no-op for rematch). */
    ear_obs_manager_feed_from_ce(&f->om, &f->ce);
    return true;
}

static void test_p2_as001_ce_om_lifecycle_e2e(void)
{
    printf("test_p2_as001_ce_om_lifecycle_e2e\n");
    fixture_t f;
    memset(&f, 0, sizeof(f));
    memset(&g_emit, 0, sizeof(g_emit));
    g_uuid_n = 1;

    bool se_listener_bound = false;
    char obs_id[SP_OBS_EPISODE_ID_LEN];
    memset(obs_id, 0, sizeof(obs_id));
    uint64_t cand_id = 0u;
    uint32_t promo_gen = 0u;
    uint32_t pre_gap_owned_seq = 0u;
    float pre_gap_owned_rms = 0.0f;
    uint8_t pre_gap_sig_count = 0u;
    uint32_t pre_gap_evidence_updates = 0u;
    uint32_t post_gap_evidence_updates = 0u;
    const float heli_rms = -40.0f;
    const float rematch_rms = -38.5f;
    const uint32_t heli_seq = 17u;
    const uint32_t gap_seq = 18u;
    const uint32_t rematch_seq = 19u;

    ae_config_t acfg;
    ae_config_set_defaults(&acfg);
    E2E_REQUIRE(ae_start(&f.ae, &acfg));

    E2E_REQUIRE(sig_engine_start());
    E2E_REQUIRE(sig_engine_is_started());
    /* Production order: Signature Engine before CE on the AE listener list. */
    E2E_REQUIRE(ae_add_signature_listener(&f.ae, sig_engine_ae_listener, NULL));
    se_listener_bound = true;

    ce_config_t ccfg;
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 1u;
    ccfg.t_coast = 3u;
    E2E_REQUIRE(ce_start(&f.ce, &ccfg));
    E2E_REQUIRE(ce_bind_audio_engine(&f.ce, &f.ae));

    sp_episode_registry_init(&f.reg);

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
    mcfg.emit_start = emit_start;
    mcfg.emit_periodic = emit_periodic;
    mcfg.emit_end = emit_end;
    mcfg.enable_poll_periodic = false;
    ear_obs_manager_init(&f.om, &mcfg);
    ce_set_promotion_cb(&f.ce, ear_obs_manager_on_promotion, &f.om);
    ear_promotion_policy_init(&f.pol, &f.ce, &f.om);

    ae_signature_t sig;

    /* Unmatched broadband must not open an Observation. */
    for (uint32_t seq = 1u; seq <= 4u; seq++) {
        fill_broadband_sig(&sig, seq, -55.0f);
        E2E_REQUIRE(e2e_drive(&f, &sig, sig.window_end_mono_us));
    }
    E2E_REQUIRE(g_emit.starts == 0u);
    E2E_REQUIRE(g_emit.ends == 0u);
    E2E_REQUIRE(ear_obs_manager_n_open(&f.om) == 0u);
    E2E_REQUIRE(ear_obs_manager_n_pending(&f.om) == 0u);
    E2E_REQUIRE(sp_episode_registry_in_use_count(&f.reg) == 0u);

    /* Fresh AS-001 heli match → exactly one Observation via Policy. */
    fill_heli_sig(&sig, heli_seq, heli_rms);
    E2E_REQUIRE(e2e_drive(&f, &sig, sig.window_end_mono_us));

    sig_primary_distance_t prim;
    memset(&prim, 0, sizeof(prim));
    E2E_REQUIRE(sig_engine_get_primary_distance_match(&prim));
    E2E_REQUIRE(prim.valid);
    E2E_REQUIRE(prim.window_seq == heli_seq);
    E2E_REQUIRE(prim.profile_id == AS_PROFILE_ID_AS001);

    E2E_REQUIRE(g_emit.starts == 1u);
    E2E_REQUIRE(ear_obs_manager_n_open(&f.om) == 1u);
    E2E_REQUIRE(sp_episode_registry_in_use_count(&f.reg) == 1u);
    E2E_REQUIRE(g_emit.last_obs_id[0] != '\0');
    E2E_REQUIRE(g_emit.last_start_cand != 0u);
    memcpy(obs_id, g_emit.last_obs_id, sizeof(obs_id));
    cand_id = g_emit.last_start_cand;

    ae_candidate_t cand;
    memset(&cand, 0, sizeof(cand));
    E2E_REQUIRE(ce_find_candidate(&f.ce, cand_id, &cand));
    E2E_REQUIRE(cand.state == AE_CAND_PROMOTED);
    E2E_REQUIRE(cand.promotion_entered);
    E2E_REQUIRE(cand.promotion_gen != 0u);
    promo_gen = cand.promotion_gen;

    ce_classification_t cls;
    memset(&cls, 0, sizeof(cls));
    E2E_REQUIRE(ce_get_classification(&f.ce, cand_id, &cls));
    E2E_REQUIRE(cls.valid && cls.matched);
    E2E_REQUIRE(cls.profile_id == AS_PROFILE_ID_AS001);
    E2E_REQUIRE(cls.distance == prim.distance);

    {
        const int bi = find_bind_idx(&f, cand_id);
        E2E_REQUIRE(bi >= 0);
        E2E_REQUIRE((uint8_t)bi < EAR_OBS_MGR_BINDING_CAP);
        E2E_REQUIRE(f.om.bind[bi].in_use);
        E2E_REQUIRE(f.om.bind[bi].ce_live);
        E2E_REQUIRE(f.om.bind[bi].ce_promotion_gen == promo_gen);
        E2E_REQUIRE(strcmp(f.om.bind[bi].observation_id, obs_id) == 0);
        E2E_REQUIRE(f.om.bind[bi].snap.product_relevance_approved);
        E2E_REQUIRE(f.om.bind[bi].snap.relevance_profile_id == AS_PROFILE_ID_AS001);

        /*
         * OM must own usable heli evidence on the Observation binding.
         * evidence_updates >= 1 proves feed_from_ce ran (ENTER alone leaves 0).
         * Owned top seq/rms pin the heli window itself.
         */
        E2E_REQUIRE(f.om.bind[bi].evidence_updates >= 1u);
        E2E_REQUIRE(f.om.bind[bi].snap.sig_count >= 1u);
        E2E_REQUIRE(f.om.bind[bi].last_owned_sig_window_seq == heli_seq);
        E2E_REQUIRE(f.om.bind[bi].snap.sigs[0].window_seq == heli_seq);
        E2E_REQUIRE(f.om.bind[bi].snap.sigs[0].quality == AE_QUALITY_OK);
        E2E_REQUIRE(f.om.bind[bi].snap.sigs[0].rms_dbfs == heli_rms);
        E2E_REQUIRE(!binding_owned_has_gap(&f.om, (uint8_t)bi));

        pre_gap_owned_seq = f.om.bind[bi].last_owned_sig_window_seq;
        pre_gap_owned_rms = f.om.bind[bi].snap.sigs[0].rms_dbfs;
        pre_gap_sig_count = f.om.bind[bi].snap.sig_count;
        pre_gap_evidence_updates = f.om.bind[bi].evidence_updates;
    }

    ear_promotion_policy_diag_t pd;
    ear_promotion_policy_get_diag(&f.pol, &pd);
    E2E_REQUIRE(pd.promote_ok >= 1u);
    E2E_REQUIRE(pd.last_decision == EAR_PROMO_DECISION_PROMOTE);

    /* Short GAP: previously owned evidence must not be overwritten by GAP. */
    fill_gap_sig(&sig, gap_seq, heli_rms);
    E2E_REQUIRE(e2e_drive(&f, &sig, sig.window_end_mono_us));

    E2E_REQUIRE(g_emit.starts == 1u);
    E2E_REQUIRE(g_emit.ends == 0u);
    E2E_REQUIRE(ear_obs_manager_n_open(&f.om) == 1u);
    memset(&cand, 0, sizeof(cand));
    E2E_REQUIRE(ce_find_candidate(&f.ce, cand_id, &cand));
    E2E_REQUIRE(cand.state == AE_CAND_COASTING);
    E2E_REQUIRE(cand.promotion_entered);
    E2E_REQUIRE(cand.promotion_gen == promo_gen);

    {
        const int bi = find_bind_idx(&f, cand_id);
        E2E_REQUIRE(bi >= 0);
        E2E_REQUIRE((uint8_t)bi < EAR_OBS_MGR_BINDING_CAP);
        E2E_REQUIRE(f.om.bind[bi].in_use);
        E2E_REQUIRE(f.om.bind[bi].ce_live);
        E2E_REQUIRE(strcmp(f.om.bind[bi].observation_id, obs_id) == 0);
        E2E_REQUIRE(f.om.bind[bi].ce_promotion_gen == promo_gen);
        E2E_REQUIRE(!binding_owned_has_gap(&f.om, (uint8_t)bi));
        E2E_REQUIRE(f.om.bind[bi].last_owned_sig_window_seq == pre_gap_owned_seq);
        E2E_REQUIRE(f.om.bind[bi].last_owned_sig_window_seq != gap_seq);
        E2E_REQUIRE(f.om.bind[bi].snap.sigs[0].window_seq == pre_gap_owned_seq);
        E2E_REQUIRE(f.om.bind[bi].snap.sigs[0].window_seq != gap_seq);
        E2E_REQUIRE(f.om.bind[bi].snap.sigs[0].rms_dbfs == pre_gap_owned_rms);
        E2E_REQUIRE(f.om.bind[bi].snap.sigs[0].quality == AE_QUALITY_OK);
        E2E_REQUIRE(f.om.bind[bi].snap.sig_count == pre_gap_sig_count);
        post_gap_evidence_updates = f.om.bind[bi].evidence_updates;
        E2E_REQUIRE(post_gap_evidence_updates >= pre_gap_evidence_updates);
    }

    /* Rematch: feed must install window-19 owned evidence on the same binding. */
    fill_heli_sig(&sig, rematch_seq, rematch_rms);
    E2E_REQUIRE(e2e_drive(&f, &sig, sig.window_end_mono_us));

    E2E_REQUIRE(g_emit.starts == 1u);
    E2E_REQUIRE(g_emit.ends == 0u);
    E2E_REQUIRE(ear_obs_manager_n_open(&f.om) == 1u);
    memset(&cand, 0, sizeof(cand));
    E2E_REQUIRE(ce_find_candidate(&f.ce, cand_id, &cand));
    E2E_REQUIRE(cand.state == AE_CAND_PROMOTED);
    E2E_REQUIRE(cand.promotion_entered);
    E2E_REQUIRE(cand.promotion_gen == promo_gen);
    E2E_REQUIRE(cand.last_window_seq == rematch_seq);

    {
        const int bi = find_bind_idx(&f, cand_id);
        E2E_REQUIRE(bi >= 0);
        E2E_REQUIRE((uint8_t)bi < EAR_OBS_MGR_BINDING_CAP);
        E2E_REQUIRE(f.om.bind[bi].in_use);
        E2E_REQUIRE(f.om.bind[bi].ce_live);
        E2E_REQUIRE(f.om.bind[bi].ce_promotion_gen == promo_gen);
        E2E_REQUIRE(strcmp(f.om.bind[bi].observation_id, obs_id) == 0);
        E2E_REQUIRE(!binding_owned_has_gap(&f.om, (uint8_t)bi));
        /*
         * These OM-owned fields change only via feed_from_ce after rematch.
         * Disabling the feed leaves last_owned at heli_seq / heli_rms.
         */
        E2E_REQUIRE(f.om.bind[bi].evidence_updates > post_gap_evidence_updates);
        E2E_REQUIRE(f.om.bind[bi].last_owned_sig_window_seq == rematch_seq);
        E2E_REQUIRE(f.om.bind[bi].snap.sigs[0].window_seq == rematch_seq);
        E2E_REQUIRE(f.om.bind[bi].snap.sigs[0].rms_dbfs == rematch_rms);
        E2E_REQUIRE(f.om.bind[bi].snap.sigs[0].quality == AE_QUALITY_OK);
        E2E_REQUIRE(f.om.bind[bi].snap.sigs[0].window_seq != gap_seq);
        E2E_REQUIRE(f.om.bind[bi].last_owned_sig_window_seq != gap_seq);
    }

    /* Coast past t_coast → natural CE EXIT detaches without END. */
    for (uint32_t seq = 20u; seq <= 23u; seq++) {
        fill_gap_sig(&sig, seq, heli_rms);
        E2E_REQUIRE(e2e_drive(&f, &sig, sig.window_end_mono_us));
    }

    E2E_REQUIRE(!ce_find_candidate(&f.ce, cand_id, &cand));
    E2E_REQUIRE(g_emit.starts == 1u);
    E2E_REQUIRE(g_emit.ends == 0u);
    E2E_REQUIRE(ear_obs_manager_n_open(&f.om) == 1u);

    const int64_t detach_now = 23LL * 1000000LL;
    {
        const int bi = find_bind_idx(&f, cand_id);
        E2E_REQUIRE(bi >= 0);
        E2E_REQUIRE((uint8_t)bi < EAR_OBS_MGR_BINDING_CAP);
        E2E_REQUIRE(f.om.bind[bi].in_use);
        E2E_REQUIRE(!f.om.bind[bi].ce_live);
        E2E_REQUIRE(f.om.bind[bi].ce_promotion_gen == promo_gen);
        E2E_REQUIRE(strcmp(f.om.bind[bi].observation_id, obs_id) == 0);
        E2E_REQUIRE(f.om.bind[bi].detached_since_mono_us == detach_now);
    }
    ear_obs_manager_diag_t d0;
    ear_obs_manager_get_diag(&f.om, &d0);
    E2E_REQUIRE(d0.exit_detach >= 1u);
    E2E_REQUIRE(d0.detached_watchdog_armed >= 1u);
    E2E_REQUIRE(d0.detached_watchdog_end == 0u);

    /* Detached watchdog deadline → exactly one END; no duplicate. */
    host_obs_poll(&f.om, detach_now + 999999LL);
    E2E_REQUIRE(g_emit.ends == 0u);
    E2E_REQUIRE(ear_obs_manager_n_open(&f.om) == 1u);

    host_obs_poll(&f.om, detach_now + 1000000LL);
    E2E_REQUIRE(g_emit.ends == 1u);
    E2E_REQUIRE(ear_obs_manager_n_open(&f.om) == 0u);
    E2E_REQUIRE(sp_episode_registry_in_use_count(&f.reg) == 0u);
    E2E_REQUIRE(strcmp(g_emit.last_end_obs_id, obs_id) == 0);
    E2E_REQUIRE(g_emit.last_end_reason == EAR_OBS_END_EXPLICIT);
    E2E_REQUIRE(g_emit.starts == 1u);

    ear_obs_manager_diag_t d1;
    ear_obs_manager_get_diag(&f.om, &d1);
    E2E_REQUIRE(d1.detached_watchdog_end == 1u);
    E2E_REQUIRE(d1.end_done == 1u);

    host_obs_poll(&f.om, detach_now + 2000000LL);
    host_obs_poll(&f.om, detach_now + 3000000LL);
    E2E_REQUIRE(g_emit.ends == 1u);
    E2E_REQUIRE(g_emit.starts == 1u);
    E2E_REQUIRE(ear_obs_manager_n_open(&f.om) == 0u);

e2e_cleanup:
    if (se_listener_bound) {
        (void)ae_remove_signature_listener(&f.ae, sig_engine_ae_listener, NULL);
    }
    ce_stop(&f.ce);
    sig_engine_stop();
}

static bool p3b_capacity_is(const fixture_t *f, uint8_t open, uint8_t pending)
{
    return ear_obs_manager_n_open(&f->om) == open
        && sp_episode_registry_in_use_count(&f->reg) == open
        && ear_obs_manager_n_pending(&f->om) == pending
        && ear_obs_manager_n_held(&f->om) == 0u;
}

/* Drive X through Policy while controlled AS-002 admissions fill both slots. */
static void test_p3b_fifo_handoff_rejects_old_generation(void)
{
    fixture_t f = {0};
    p3b_events_t events = {0};
    bool se_bound = false;
    bool om_ready = false;
    const uint64_t b = 101u;
    const uint64_t c = 102u;
    const int64_t detach_time = 17400000;
    const int64_t deadline = detach_time + 1000000;
    const int64_t replay_time = 18700000;
    g_emit = (emit_counts_t){0};
    g_captured_exit_n = 0u;
    g_uuid_n = 1u;

    ae_config_t acfg;
    ae_config_set_defaults(&acfg);
    E2E_REQUIRE(ae_start(&f.ae, &acfg));
    E2E_REQUIRE(sig_engine_start());
    E2E_REQUIRE(ae_add_signature_listener(&f.ae, sig_engine_ae_listener, NULL));
    se_bound = true;

    ce_config_t ccfg;
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 1u;
    ccfg.t_coast = 3u;
    E2E_REQUIRE(ce_start(&f.ce, &ccfg));
    E2E_REQUIRE(ce_bind_audio_engine(&f.ce, &f.ae));
    sp_episode_registry_init(&f.reg);
    const ear_obs_manager_config_t mcfg = {
        .registry = &f.reg,
        .ce = &f.ce,
        .ae = &f.ae,
        .uuid_fn = fake_uuid,
        .observation_interval_ms = 1000u,
        .pending_timeout_ms = 30000u,
        .detached_end_timeout_ms = 1000u,
        .as002_signature_absence_hold_ms = 2000u,
        .producer_tag = 2u,
        .emit_ctx = &events,
        .emit_start = emit_start,
        .emit_end = emit_end,
    };
    ear_obs_manager_init(&f.om, &mcfg);
    om_ready = true;
    ear_promotion_policy_init(&f.pol, &f.ce, &f.om);
    ce_set_promotion_cb(&f.ce, om_promo_capture_exit, &f.om);

    ae_signature_t sig;
    for (uint32_t seq = 1u; seq <= 4u; seq++) {
        fill_broadband_sig(&sig, seq, -55.0f);
        E2E_REQUIRE(e2e_drive(&f, &sig, sig.window_end_mono_us));
    }
    E2E_REQUIRE(events.count == 0u);
    fill_heli_sig(&sig, 17u, -40.0f);
    E2E_REQUIRE(e2e_drive(&f, &sig, sig.window_end_mono_us));
    E2E_REQUIRE(events.count == 1u);
    E2E_REQUIRE(events.entries[0].start);
    const uint64_t x = events.entries[0].candidate_id;
    ae_candidate_t live = {0};
    E2E_REQUIRE(ce_find_candidate(&f.ce, x, &live));
    const uint32_t g1 = live.promotion_gen;
    E2E_REQUIRE(g1 != 0u);
    const int old_slot = find_bind_idx(&f, x);
    E2E_REQUIRE(old_slot >= 0 && (unsigned)old_slot < EAR_OBS_MGR_BINDING_CAP);
    E2E_REQUIRE(f.om.bind[old_slot].ce_promotion_gen == g1);
    E2E_REQUIRE(f.om.bind[old_slot].evidence_updates > 0u);
    E2E_REQUIRE(f.om.bind[old_slot].snap.sig_count > 0u);
    E2E_REQUIRE(f.om.bind[old_slot].snap.sigs[0].window_seq == 17u);
    E2E_REQUIRE(f.om.bind[old_slot].last_owned_sig_window_seq == 17u);

    ae_candidate_t *fixture = plant(&f.ce, b, AE_CAND_ACTIVE, 4u, -31.0f);
    E2E_REQUIRE(fixture != NULL);
    fixture->last_evidence_mono_us = 17100000;
    E2E_REQUIRE(promote_as002_with_reserve(&f, b, 17100000));
    E2E_REQUIRE(events.count == 2u);
    E2E_REQUIRE(events.entries[1].start && events.entries[1].candidate_id == b);
    const int b_slot = find_bind_idx(&f, b);
    E2E_REQUIRE(b_slot >= 0 && b_slot != old_slot);
    fixture = plant(&f.ce, c, AE_CAND_ACTIVE, 4u, -32.0f);
    E2E_REQUIRE(fixture != NULL);
    fixture->last_evidence_mono_us = 17200000;
    E2E_REQUIRE(promote_as002_with_reserve(&f, c, 17200000));
    E2E_REQUIRE(events.count == 2u && find_bind_idx(&f, c) < 0);
    E2E_REQUIRE(p3b_capacity_is(&f, 2u, 1u));
    E2E_REQUIRE(f.om.pending_order[0] < EAR_OBS_MGR_PENDING_CAP);
    E2E_REQUIRE(f.om.pending[f.om.pending_order[0]].candidate_id == c);
    ear_obs_manager_on_as002_siren_window(
        &f.om, true, false, 1000u, 17300000, true, -31.0f
    );
    E2E_REQUIRE(events.count == 2u);

    E2E_REQUIRE(ce_unadmit_promotion(&f.ce, x, g1));
    E2E_REQUIRE(g_captured_exit_n == 1u);
    E2E_REQUIRE(g_captured_exit.candidate_id == x && g_captured_exit.promotion_gen == g1);
    host_obs_poll(&f.om, detach_time);
    E2E_REQUIRE(events.count == 2u && p3b_capacity_is(&f, 2u, 1u));
    E2E_REQUIRE(!f.om.bind[old_slot].ce_live);
    E2E_REQUIRE(f.om.bind[old_slot].detached_since_mono_us == detach_time);

    fill_heli_sig(&sig, 18u, -39.0f);
    E2E_REQUIRE(e2e_drive(&f, &sig, sig.window_end_mono_us));
    E2E_REQUIRE(ce_find_candidate(&f.ce, x, &live));
    E2E_REQUIRE(live.state == AE_CAND_ACTIVE && !live.promotion_entered);
    host_obs_poll(&f.om, deadline - 1);
    E2E_REQUIRE(events.count == 2u && p3b_capacity_is(&f, 2u, 1u));
    host_obs_poll(&f.om, deadline);
    E2E_REQUIRE(events.count == 4u);
    E2E_REQUIRE(!events.entries[2].start && events.entries[2].candidate_id == x);
    E2E_REQUIRE(strcmp(events.entries[2].id, events.entries[0].id) == 0);
    E2E_REQUIRE(g_emit.last_end_reason == EAR_OBS_END_EXPLICIT);
    E2E_REQUIRE(events.entries[3].start && events.entries[3].candidate_id == c);
    E2E_REQUIRE(find_bind_idx(&f, c) == old_slot && find_bind_idx(&f, b) >= 0);
    E2E_REQUIRE(p3b_capacity_is(&f, 2u, 0u));

    ear_promotion_policy_poll(&f.pol, 18500000);
    host_obs_poll(&f.om, 18500000);
    E2E_REQUIRE(ce_find_candidate(&f.ce, x, &live));
    const uint32_t g2 = live.promotion_gen;
    E2E_REQUIRE(live.state == AE_CAND_PROMOTED && g2 != 0u && g2 != g1);
    E2E_REQUIRE(events.count == 4u && find_bind_idx(&f, x) < 0);
    E2E_REQUIRE(p3b_capacity_is(&f, 2u, 1u));
    E2E_REQUIRE(f.om.pending_order[0] < EAR_OBS_MGR_PENDING_CAP);
    E2E_REQUIRE(f.om.pending[f.om.pending_order[0]].candidate_id == x);
    E2E_REQUIRE(f.om.pending[f.om.pending_order[0]].snap.promotion_gen == g2);

    ear_obs_manager_on_as002_siren_window(
        &f.om, true, false, 1000u, 18600000, true, -31.0f
    );
    E2E_REQUIRE(events.count == 6u);
    E2E_REQUIRE(!events.entries[4].start && events.entries[4].candidate_id == b);
    E2E_REQUIRE(strcmp(events.entries[4].id, events.entries[1].id) == 0);
    E2E_REQUIRE(g_emit.last_end_reason == EAR_OBS_END_SIGNATURE);
    E2E_REQUIRE(events.entries[5].start && events.entries[5].candidate_id == x);
    E2E_REQUIRE(strcmp(events.entries[5].id, events.entries[0].id) != 0);
    E2E_REQUIRE(p3b_capacity_is(&f, 2u, 0u));
    const int slot = find_bind_idx(&f, x);
    E2E_REQUIRE(slot >= 0 && (unsigned)slot < EAR_OBS_MGR_BINDING_CAP);
    E2E_REQUIRE(slot == b_slot);
    E2E_REQUIRE(f.om.bind[slot].ce_live && f.om.bind[slot].ce_promotion_gen == g2);
    E2E_REQUIRE(f.om.bind[slot].detached_since_mono_us == 0);
    E2E_REQUIRE(f.om.bind[slot].snap.sig_count > 0u);
    E2E_REQUIRE(f.om.bind[slot].snap.sigs[0].window_seq == 18u);
    E2E_REQUIRE(f.om.bind[slot].last_owned_sig_window_seq == 18u);

    ear_obs_manager_on_promotion(&g_captured_exit, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, replay_time);
    E2E_REQUIRE(events.count == 6u && p3b_capacity_is(&f, 2u, 0u));
    E2E_REQUIRE(find_bind_idx(&f, x) == slot && f.om.bind[slot].ce_live);
    E2E_REQUIRE(f.om.bind[slot].detached_since_mono_us == 0);
    E2E_REQUIRE(f.om.diag.exit_stale == 1u);

    ear_obs_promote_snapshot_t before_stale;
    memcpy(&before_stale, &f.om.bind[slot].snap, sizeof(before_stale));
    const uint32_t updates = f.om.bind[slot].evidence_updates;
    const uint32_t dropped = f.om.diag.evidence_dropped;
    ae_candidate_t stale = g_captured_exit;
    fill_heli_sig(&sig, 20u, -35.0f);
    stale.last_window_seq = sig.window_seq;
    stale.last_evidence_mono_us = sig.window_end_mono_us;
    stale.ema_rms_dbfs = sig.rms_dbfs;
    ear_obs_manager_on_candidate_evidence(&f.om, x, &sig, &stale);
    E2E_REQUIRE(f.om.bind[slot].last_owned_sig_window_seq == 18u);
    E2E_REQUIRE(f.om.bind[slot].evidence_updates == updates);
    E2E_REQUIRE(f.om.diag.evidence_dropped == dropped + 1u);
    E2E_REQUIRE(memcmp(&before_stale, &f.om.bind[slot].snap, sizeof(before_stale)) == 0);

    fill_heli_sig(&sig, 19u, -38.5f);
    E2E_REQUIRE(e2e_drive(&f, &sig, sig.window_end_mono_us));
    E2E_REQUIRE(f.om.bind[slot].last_owned_sig_window_seq == 19u);
    E2E_REQUIRE(f.om.bind[slot].snap.sigs[0].window_seq == 19u);
    E2E_REQUIRE(f.om.bind[slot].snap.sigs[0].rms_dbfs == sig.rms_dbfs);
    E2E_REQUIRE(f.om.bind[slot].evidence_updates > updates);
    host_obs_poll(&f.om, replay_time + 1000001);
    E2E_REQUIRE(events.count == 6u && g_emit.starts == 4u && g_emit.ends == 2u);
    E2E_REQUIRE(p3b_capacity_is(&f, 2u, 0u) && find_bind_idx(&f, c) == old_slot);
    E2E_REQUIRE(find_bind_idx(&f, x) == slot && f.om.bind[slot].ce_live);
    E2E_REQUIRE(f.om.bind[slot].ce_promotion_gen == g2);
    E2E_REQUIRE(strcmp(f.om.bind[slot].observation_id, events.entries[5].id) == 0);

e2e_cleanup:
    if (se_bound) {
        (void)ae_remove_signature_listener(&f.ae, sig_engine_ae_listener, NULL);
    }
    ce_set_promotion_cb(&f.ce, NULL, NULL);
    ce_stop(&f.ce);
    sig_engine_stop();
    if (om_ready) {
        ear_obs_manager_forget_bindings(&f.om);
        sp_episode_registry_init(&f.reg);
    }
    g_captured_exit = (ae_candidate_t){0};
    g_captured_exit_n = 0u;
    g_emit = (emit_counts_t){0};
    g_uuid_n = 0u;
}
#undef E2E_REQUIRE

static void test_p7_start_retry_same_identity(void)
{
    fixture_t f;
    fixture_init(&f, false);
    g_emit.fail_start_remaining = 1u;
    plant(&f.ce, 7001u, AE_CAND_ACTIVE, 4u, -30.0f);
    /* handle_enter freezes START at last_evidence_mono_us (plant: id * 1000). */
    const int64_t t0 = 7001LL * 1000;
    EXPECT(promote_with_reserve(&f, 7001u, t0));
    const int bi = find_bind_idx(&f, 7001u);
    EXPECT(bi >= 0);
    EXPECT(g_emit.starts == 1u);
    EXPECT(g_emit.sample_n == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(!f.reg.slots[f.om.bind[bi].ep.index].start_publish_accepted);
    EXPECT(f.om.bind[bi].start_frozen);
    EXPECT(f.om.bind[bi].start_capture_mono_us == t0);
    EXPECT(f.om.bind[bi].start_retry_not_before_mono_us == t0 + 1000000);

    const uint16_t gen = f.om.bind[bi].ep.generation;
    f.om.bind[bi].snap.ema_rms_dbfs = 12.5f;
    f.om.bind[bi].snap.last_window_seq += 9u;
    f.om.bind[bi].cls_wire.present = true;
    f.om.bind[bi].cls_wire.n_nodes = 1u;
    snprintf(f.om.bind[bi].cls_wire.nodes[0].id, EAR_CLS_NODE_ID_MAX, "SIREN");

    host_obs_poll(&f.om, t0 + 999999);
    EXPECT(g_emit.starts == 1u);

    host_obs_poll(&f.om, t0 + 1000000);
    EXPECT(g_emit.starts == 2u);
    EXPECT(g_emit.sample_n == 2u);
    EXPECT(strcmp(g_emit.sample[0].id, g_emit.sample[1].id) == 0);
    EXPECT(g_emit.sample[0].gen == g_emit.sample[1].gen);
    EXPECT(g_emit.sample[1].gen == gen);
    EXPECT(g_emit.sample[0].capture == g_emit.sample[1].capture);
    EXPECT(g_emit.sample[1].capture == t0);
    EXPECT(g_emit.sample[0].ema == g_emit.sample[1].ema);
    EXPECT(g_emit.sample[1].ema != 12.5f);
    EXPECT(g_emit.sample[0].seq == g_emit.sample[1].seq);
    EXPECT(g_emit.sample[0].sig_count == g_emit.sample[1].sig_count);
    EXPECT(strcmp(g_emit.sample[0].assess, g_emit.sample[1].assess) == 0);
    EXPECT(strcmp(g_emit.sample[1].assess, "AIRCRAFT") == 0);
    EXPECT(f.reg.slots[f.om.bind[bi].ep.index].start_publish_accepted);
    EXPECT(f.om.bind[bi].ep.generation == gen);
    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.start_retry == 1u);
    fixture_fini(&f);
}

static void test_p7_assessment_frozen_across_retry(void)
{
    fixture_t as2;
    fixture_init(&as2, false);
    g_emit.fail_start_remaining = 1u;
    plant(&as2.ce, 7002u, AE_CAND_ACTIVE, 4u, -30.0f);
    const int64_t t0 = 7002LL * 1000;
    EXPECT(promote_as002_with_reserve(&as2, 7002u, t0));
    const int bi = find_bind_idx(&as2, 7002u);
    EXPECT(bi >= 0);
    EXPECT(!g_emit.sample[0].assess_present);
    EXPECT(g_emit.sample[0].assess[0] == '\0');
    as2.om.bind[bi].cls_wire.present = true;
    as2.om.bind[bi].cls_wire.n_nodes = 1u;
    snprintf(as2.om.bind[bi].cls_wire.nodes[0].id, EAR_CLS_NODE_ID_MAX, "SIREN");
    host_obs_poll(&as2.om, t0 + 1000000);
    EXPECT(g_emit.starts == 2u);
    EXPECT(!g_emit.sample[1].assess_present);
    EXPECT(g_emit.sample[1].assess[0] == '\0');
    EXPECT(g_emit.sample[1].capture == t0);
    fixture_fini(&as2);
}

static void test_p7_retry_spacing_and_no_duplicate(void)
{
    fixture_t f;
    fixture_init(&f, false);
    g_emit.fail_start_remaining = 1u;
    plant(&f.ce, 7003u, AE_CAND_ACTIVE, 3u, -28.0f);
    const int64_t t0 = 7003LL * 1000;
    EXPECT(promote_with_reserve(&f, 7003u, t0));
    for (int64_t t = t0 + 100000; t < t0 + 1000000; t += 100000) {
        host_obs_poll(&f.om, t);
    }
    EXPECT(g_emit.starts == 1u);
    host_obs_poll(&f.om, t0 + 1000000);
    EXPECT(g_emit.starts == 2u);
    const uint32_t accepted = g_emit.starts;
    for (int i = 0; i < 20; i++) {
        host_obs_poll(&f.om, t0 + 1000000 + (int64_t)i * 100000);
    }
    EXPECT(g_emit.starts == accepted);
    fixture_fini(&f);
}

static void test_p7_periodic_suppressed_both_paths(void)
{
    fixture_t f;
    fixture_init(&f, false);
    g_emit.fail_start_remaining = 1u;
    plant(&f.ce, 7004u, AE_CAND_ACTIVE, 4u, -30.0f);
    const int64_t t0 = 7004LL * 1000;
    EXPECT(promote_with_reserve(&f, 7004u, t0));
    ear_obs_manager_on_activity_periodic_hint(&f.om, t0 - 1000000, t0, true, -36.0f);
    EXPECT(g_emit.periodics == 0u);
    f.om.cfg.observation_interval_ms = 100u;
    host_obs_poll(&f.om, t0 + 100000);
    EXPECT(g_emit.periodics == 0u);
    EXPECT(g_emit.starts == 1u);
    host_obs_poll(&f.om, t0 + 1000000);
    EXPECT(g_emit.starts == 2u);
    EXPECT(g_emit.periodics == 0u);
    ear_obs_manager_on_activity_periodic_hint(&f.om, 3000000, 4000000, true, -36.0f);
    EXPECT(g_emit.periodics == 1u);
    fixture_fini(&f);
}

static void test_p7_silent_end_before_accept(void)
{
    fixture_t f;
    fixture_init(&f, false);
    g_emit.fail_start_remaining = 1u;
    plant(&f.ce, 7005u, AE_CAND_ACTIVE, 4u, -30.0f);
    const int64_t t0 = 7005LL * 1000;
    EXPECT(promote_with_reserve(&f, 7005u, t0));
    ear_obs_manager_on_activity_end_hint(
        &f.om, EAR_OBS_ACT_END_AMBIENT, t0, t0 + 100000, true, -50.0f
    );
    EXPECT(g_emit.ends == 0u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);
    host_obs_poll(&f.om, t0 + 5000000);
    EXPECT(g_emit.starts == 1u);
    EXPECT(g_emit.ends == 0u);

    g_emit.fail_start_remaining = 1u;
    plant(&f.ce, 7006u, AE_CAND_ACTIVE, 4u, -29.0f);
    const int64_t t1 = 7006LL * 1000;
    EXPECT(promote_with_reserve(&f, 7006u, t1));
    ear_obs_manager_force_end_all(&f.om, EAR_OBS_END_EXPLICIT, t1 + 100000);
    EXPECT(g_emit.ends == 0u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);
    fixture_fini(&f);
}

static void test_p7_exit_and_watchdog_before_accept(void)
{
    fixture_t stale;
    fixture_init(&stale, false);
    stale.om.cfg.detached_end_timeout_ms = 1000u;
    g_emit.fail_start_remaining = 1u;
    plant(&stale.ce, 7010u, AE_CAND_ACTIVE, 4u, -30.0f);
    const int64_t ts = 7010LL * 1000;
    EXPECT(promote_with_reserve(&stale, 7010u, ts));
    const int bi = find_bind_idx(&stale, 7010u);
    EXPECT(bi >= 0);
    ae_candidate_t c;
    EXPECT(ce_find_candidate(&stale.ce, 7010u, &c));
    c.promotion_gen = stale.om.bind[bi].ce_promotion_gen + 99u;
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &stale.om);
    host_obs_poll(&stale.om, ts + 100000);
    EXPECT(stale.om.bind[bi].ce_live);
    EXPECT(g_emit.ends == 0u);
    ear_obs_manager_diag_t ds;
    ear_obs_manager_get_diag(&stale.om, &ds);
    EXPECT(ds.exit_stale >= 1u);
    host_obs_poll(&stale.om, ts + 1000000);
    EXPECT(g_emit.starts == 2u);
    EXPECT(g_emit.ends == 0u);
    EXPECT(ear_obs_manager_n_open(&stale.om) == 1u);
    fixture_fini(&stale);

    fixture_t f;
    fixture_init(&f, false);
    f.om.cfg.detached_end_timeout_ms = 1000u;
    g_emit.fail_start_remaining = 1u;
    plant(&f.ce, 7011u, AE_CAND_ACTIVE, 4u, -30.0f);
    const int64_t t0 = 7011LL * 1000;
    EXPECT(promote_with_reserve(&f, 7011u, t0));
    EXPECT(ce_find_candidate(&f.ce, 7011u, &c));
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &f.om);
    host_obs_poll(&f.om, t0);
    const int live = find_bind_idx(&f, 7011u);
    EXPECT(live >= 0);
    EXPECT(!f.om.bind[live].ce_live);
    EXPECT(f.om.bind[live].detached_since_mono_us == t0);
    EXPECT(g_emit.ends == 0u);
    EXPECT(g_emit.starts == 1u);
    host_obs_poll(&f.om, t0 + 1000000);
    EXPECT(g_emit.ends == 0u);
    EXPECT(g_emit.starts == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.detached_watchdog_end == 1u);
    EXPECT(d.start_retry == 0u);
    fixture_fini(&f);
}

static void test_p7_as002_absence_and_invalid_hold_before_accept(void)
{
    fixture_t abs;
    fixture_init(&abs, false);
    abs.om.cfg.as002_signature_absence_hold_ms = 2000u;
    g_emit.fail_start_remaining = 1u;
    plant(&abs.ce, 7020u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_as002_with_reserve(&abs, 7020u, 1000));
    ear_obs_manager_on_as002_siren_window(
        &abs.om, true, false, 1000u, 6000, true, -40.0f
    );
    ear_obs_manager_on_as002_siren_window(
        &abs.om, true, false, 1000u, 7000, true, -40.0f
    );
    EXPECT(g_emit.ends == 0u);
    EXPECT(ear_obs_manager_n_open(&abs.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&abs.reg) == 0u);
    fixture_fini(&abs);

    fixture_t hold;
    fixture_init(&hold, false);
    g_emit.fail_start_remaining = 1u;
    plant(&hold.ce, 7021u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_as002_with_reserve(&hold, 7021u, 1000));
    ear_obs_manager_on_activity_end_hint(
        &hold.om, EAR_OBS_ACT_END_INVALID_HOLD, 1000, 2000, true, -55.0f
    );
    EXPECT(g_emit.ends == 0u);
    EXPECT(ear_obs_manager_n_open(&hold.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&hold.reg) == 0u);
    fixture_fini(&hold);
}

static void test_p7_two_slots_independent(void)
{
    fixture_t f;
    fixture_init(&f, false);
    g_emit.fail_start_remaining = 1u;
    plant(&f.ce, 7030u, AE_CAND_ACTIVE, 4u, -30.0f);
    plant(&f.ce, 7031u, AE_CAND_ACTIVE, 4u, -31.0f);
    const int64_t t_a = 7030LL * 1000;
    const int64_t t_b = 7031LL * 1000;
    EXPECT(promote_with_reserve(&f, 7030u, t_a));
    EXPECT(promote_with_reserve(&f, 7031u, t_b));
    EXPECT(g_emit.starts == 2u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);
    const int ia = find_bind_idx(&f, 7030u);
    const int ib = find_bind_idx(&f, 7031u);
    EXPECT(ia >= 0 && ib >= 0);
    EXPECT(!f.reg.slots[f.om.bind[ia].ep.index].start_publish_accepted);
    EXPECT(f.reg.slots[f.om.bind[ib].ep.index].start_publish_accepted);
    ear_obs_manager_on_activity_periodic_hint(&f.om, t_a, t_b, true, -34.0f);
    EXPECT(g_emit.periodics == 1u);
    EXPECT(g_emit.last_periodic_cand == 7031u);
    host_obs_poll(&f.om, t_a + 999999);
    EXPECT(g_emit.starts == 2u);
    host_obs_poll(&f.om, t_a + 1000000);
    EXPECT(g_emit.starts == 3u);
    EXPECT(strcmp(g_emit.sample[2].id, g_emit.sample[0].id) == 0);
    EXPECT(g_emit.sample[2].gen == g_emit.sample[0].gen);
    EXPECT(f.reg.slots[f.om.bind[ib].ep.index].start_publish_accepted);
    EXPECT(strcmp(f.om.bind[ib].observation_id, g_emit.sample[1].id) == 0);
    fixture_fini(&f);
}

static void test_p7_stale_handle_and_fifo(void)
{
    fixture_t stale;
    fixture_init(&stale, false);
    g_emit.fail_start_remaining = 1u;
    plant(&stale.ce, 7040u, AE_CAND_ACTIVE, 4u, -30.0f);
    const int64_t t_stale = 7040LL * 1000;
    EXPECT(promote_with_reserve(&stale, 7040u, t_stale));
    const int bi = find_bind_idx(&stale, 7040u);
    EXPECT(bi >= 0);
    char kept_id[SP_OBS_EPISODE_ID_LEN];
    memcpy(kept_id, stale.reg.slots[stale.om.bind[bi].ep.index].observation_id, sizeof(kept_id));
    stale.om.bind[bi].ep.generation = (uint16_t)(stale.om.bind[bi].ep.generation + 50u);
    host_obs_poll(&stale.om, t_stale + 1000000);
    EXPECT(g_emit.starts == 1u);
    EXPECT(stale.reg.slots[0].in_use || stale.reg.slots[1].in_use);
    EXPECT(sp_episode_registry_in_use_count(&stale.reg) == 1u);
    EXPECT(strcmp(stale.reg.slots[0].observation_id, kept_id) == 0
        || strcmp(stale.reg.slots[1].observation_id, kept_id) == 0);
    ear_obs_manager_on_activity_end_hint(
        &stale.om, EAR_OBS_ACT_END_AMBIENT, 2000000, 2100000, true, -50.0f
    );
    EXPECT(g_emit.ends == 0u);
    EXPECT(sp_episode_registry_in_use_count(&stale.reg) == 1u);
    EXPECT(strcmp(stale.reg.slots[0].observation_id, kept_id) == 0
        || strcmp(stale.reg.slots[1].observation_id, kept_id) == 0);
    fixture_fini(&stale);

    fixture_t reuse;
    fixture_init(&reuse, false);
    g_emit.fail_start_remaining = 1u;
    plant(&reuse.ce, 7041u, AE_CAND_ACTIVE, 4u, -30.0f);
    const int64_t t_reuse = 7041LL * 1000;
    EXPECT(promote_with_reserve(&reuse, 7041u, t_reuse));
    const int old = find_bind_idx(&reuse, 7041u);
    EXPECT(old >= 0);
    const sp_ep_handle_t old_ep = reuse.om.bind[old].ep;
    sp_episode_registry_release(&reuse.reg, old_ep);
    sp_ep_handle_t neu;
    EXPECT(sp_episode_registry_allocate(
        &reuse.reg, &neu, 3000000, 1000u, 2u, fake_uuid, false
    ));
    char neu_id[SP_OBS_EPISODE_ID_LEN];
    memcpy(neu_id, reuse.reg.slots[neu.index].observation_id, sizeof(neu_id));
    EXPECT(neu.generation != old_ep.generation);
    host_obs_poll(&reuse.om, t_reuse + 1000000);
    EXPECT(g_emit.starts == 1u);
    EXPECT(reuse.reg.slots[neu.index].in_use);
    EXPECT(strcmp(reuse.reg.slots[neu.index].observation_id, neu_id) == 0);
    EXPECT(!reuse.reg.slots[neu.index].start_publish_accepted);
    ear_obs_manager_on_activity_end_hint(
        &reuse.om, EAR_OBS_ACT_END_AMBIENT, 2000000, 2100000, true, -50.0f
    );
    EXPECT(g_emit.ends == 0u);
    EXPECT(reuse.reg.slots[neu.index].in_use);
    EXPECT(strcmp(reuse.reg.slots[neu.index].observation_id, neu_id) == 0);
    fixture_fini(&reuse);

    fixture_t fifo;
    fixture_init(&fifo, false);
    g_emit.fail_start_remaining = 2u;
    plant(&fifo.ce, 7042u, AE_CAND_ACTIVE, 4u, -30.0f);
    plant(&fifo.ce, 7043u, AE_CAND_ACTIVE, 4u, -31.0f);
    plant(&fifo.ce, 7044u, AE_CAND_ACTIVE, 4u, -32.0f);
    EXPECT(promote_with_reserve(&fifo, 7042u, 1000000));
    EXPECT(promote_with_reserve(&fifo, 7043u, 1100000));
    EXPECT(promote_with_reserve(&fifo, 7044u, 1200000));
    EXPECT(ear_obs_manager_n_open(&fifo.om) == 2u);
    EXPECT(ear_obs_manager_n_pending(&fifo.om) == 1u);
    EXPECT(g_emit.starts == 2u);
    ear_obs_manager_on_activity_end_hint(
        &fifo.om, EAR_OBS_ACT_END_AMBIENT, 1200000, 1300000, true, -50.0f
    );
    EXPECT(g_emit.ends == 0u);
    EXPECT(g_emit.starts == 3u);
    EXPECT(ear_obs_manager_n_open(&fifo.om) == 1u);
    EXPECT(ear_obs_manager_n_pending(&fifo.om) == 0u);
    EXPECT(find_bind_idx(&fifo, 7044u) >= 0);
    EXPECT(g_emit.last_start_cand == 7044u);
    fixture_fini(&fifo);
}

static void test_p7_ota_and_lock_and_corrupt_retry(void)
{
    fixture_t before;
    fixture_init(&before, false);
    g_emit.fail_start_remaining = 1u;
    plant(&before.ce, 7050u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&before, 7050u, 1000000));
    ear_obs_manager_forget_bindings(&before.om);
    host_obs_poll(&before.om, 1000000 + 10000000);
    EXPECT(g_emit.starts == 1u);
    EXPECT(g_emit.ends == 0u);
    EXPECT(ear_obs_manager_n_open(&before.om) == 0u);
    fixture_fini(&before);

    fixture_t after;
    fixture_init(&after, false);
    plant(&after.ce, 7051u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&after, 7051u, 1000000));
    const int bi = find_bind_idx(&after, 7051u);
    EXPECT(bi >= 0);
    const uint8_t accepted_index = after.om.bind[bi].ep.index;
    EXPECT(after.reg.slots[accepted_index].start_publish_accepted);
    ear_obs_manager_forget_bindings(&after.om);
    EXPECT(g_emit.ends == 0u);
    EXPECT(after.reg.slots[accepted_index].in_use);
    EXPECT(after.reg.slots[accepted_index].start_publish_accepted);
    host_obs_poll(&after.om, 1000000 + 10000000);
    EXPECT(g_emit.starts == 1u);
    fixture_fini(&after);

    /*
     * Negative injections. These fail if the deadline guard or the
     * one-attempt-per-poll guard is removed.
     */
    fixture_t zero;
    fixture_init(&zero, false);
    g_emit.fail_start_remaining = 1u;
    plant(&zero.ce, 7052u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&zero, 7052u, 1000000));
    const int zi = find_bind_idx(&zero, 7052u);
    EXPECT(zi >= 0);
    zero.om.bind[zi].start_retry_not_before_mono_us = 0;
    host_obs_poll(&zero.om, 1000000 + 10000000);
    EXPECT(g_emit.starts == 1u);
    ear_obs_manager_diag_t zd;
    ear_obs_manager_get_diag(&zero.om, &zd);
    EXPECT(zd.start_retry == 0u);
    fixture_fini(&zero);

    fixture_t ancient;
    fixture_init(&ancient, false);
    g_emit.fail_start_remaining = 5u;
    plant(&ancient.ce, 7053u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&ancient, 7053u, 1000000));
    EXPECT(g_emit.starts == 1u);
    EXPECT(g_emit.fail_start_remaining == 4u);
    const int ai = find_bind_idx(&ancient, 7053u);
    EXPECT(ai >= 0);
    ancient.om.bind[ai].start_retry_not_before_mono_us = 1;
    host_obs_poll(&ancient.om, 1000000 + 10000000);
    EXPECT(g_emit.starts == 2u);
    EXPECT(g_emit.fail_start_remaining == 3u);
    fixture_fini(&ancient);
}

static void test_p7_measure_retain_bytes(void)
{
    ear_obs_start_retain_reset();
    ear_obs_start_measure_t in;
    memset(&in, 0, sizeof(in));
    in.capture_mono_us = 42;
    in.interval_start_mono_us = 7;
    in.l0.rms_mean_dbfs = -33.25f;
    in.l0.has_rms_stats = true;
    in.l0.window_count = 4u;
    in.have_l1 = true;
    in.l1.interval_start_mono_us = 7;
    in.have_l2 = false;
    ear_obs_start_retain_put("obs-a", 3u, &in);
    in.l0.rms_mean_dbfs = 0.0f;
    in.capture_mono_us = 99;
    ear_obs_start_measure_t out;
    memset(&out, 0, sizeof(out));
    EXPECT(ear_obs_start_retain_find("obs-a", 3u, &out));
    EXPECT(out.capture_mono_us == 42);
    EXPECT(out.interval_start_mono_us == 7);
    EXPECT(out.l0.rms_mean_dbfs == -33.25f);
    EXPECT(out.l0.window_count == 4u);
    EXPECT(out.have_l1);
    EXPECT(!ear_obs_start_retain_find("obs-a", 4u, &out));
    ear_obs_start_retain_drop("obs-a", 3u);
    EXPECT(!ear_obs_start_retain_find("obs-a", 3u, &out));

    ear_obs_start_measure_t b;
    memset(&b, 0, sizeof(b));
    b.capture_mono_us = 2;
    b.l0.rms_mean_dbfs = -10.0f;
    EXPECT(ear_obs_start_retain_put("obs-b", 1u, &in));
    EXPECT(ear_obs_start_retain_put("obs-c", 1u, &b));
    EXPECT(ear_obs_start_retain_find("obs-c", 1u, &out));
    EXPECT(out.l0.rms_mean_dbfs == -10.0f);
    EXPECT(!ear_obs_start_retain_find("obs-c", 2u, &out));
    ear_obs_start_measure_t third = b;
    third.l0.rms_mean_dbfs = -1.0f;
    EXPECT(!ear_obs_start_retain_put("obs-d", 1u, &third));
    EXPECT(ear_obs_start_retain_find("obs-c", 1u, &out));
    EXPECT(out.l0.rms_mean_dbfs == -10.0f);
    ear_obs_start_retain_drop("obs-b", 1u);
    EXPECT(ear_obs_start_retain_put("obs-d", 1u, &third));
    EXPECT(ear_obs_start_retain_find("obs-d", 1u, &out));
    EXPECT(out.l0.rms_mean_dbfs == -1.0f);
    EXPECT(ear_obs_start_retain_find("obs-c", 1u, &out));
    EXPECT(out.l0.rms_mean_dbfs == -10.0f);
    ear_obs_start_retain_reset();
}

static void age_evidence(ce_engine_t *ce, uint64_t id, int64_t evidence_us)
{
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (ce->slots[i].in_use && ce->slots[i].candidate_id == id) {
            ce->slots[i].last_evidence_mono_us = evidence_us;
            return;
        }
    }
    EXPECT(0 && "candidate missing");
}

static void test_p7_attempt_clock(void)
{
    fixture_t f;
    fixture_init(&f, false);
    g_emit.fail_start_remaining = 4u;
    g_start_callback_consume_us = 3000000;
    plant(&f.ce, 7100u, AE_CAND_ACTIVE, 4u, -30.0f);
    const int64_t poll0 = 20000000;
    const int64_t capture = poll0 - 5000000;
    age_evidence(&f.ce, 7100u, capture);
    EXPECT(promote_with_reserve(&f, 7100u, poll0));
    const int bi = find_bind_idx(&f, 7100u);
    EXPECT(bi >= 0);
    EXPECT(g_emit.starts == 1u);
    EXPECT(f.om.bind[bi].start_capture_mono_us == capture);
    EXPECT(g_emit.sample[0].capture == capture);
    EXPECT(f.om.bind[bi].start_last_attempt_mono_us == poll0);
    EXPECT(f.om.bind[bi].start_retry_not_before_mono_us == poll0 + 1000000);
    EXPECT(f.om.bind[bi].start_retry_not_before_mono_us != capture + 1000000);
    EXPECT(f.om.bind[bi].start_retry_not_before_mono_us
           != g_clock_after_start_callback + 1000000);

    host_obs_poll(&f.om, poll0 + 999999);
    EXPECT(g_emit.starts == 1u);
    host_obs_poll(&f.om, poll0 + 1000000);
    EXPECT(g_emit.starts == 2u);
    EXPECT(f.om.bind[bi].start_capture_mono_us == capture);
    EXPECT(g_emit.sample[1].capture == capture);
    EXPECT(f.om.bind[bi].start_last_attempt_mono_us == poll0 + 1000000);
    EXPECT(f.om.bind[bi].start_retry_not_before_mono_us == poll0 + 2000000);
    host_obs_poll(&f.om, poll0 + 1999999);
    EXPECT(g_emit.starts == 2u);
    fixture_fini(&f);

    fixture_t late;
    fixture_init(&late, false);
    g_emit.fail_start_remaining = 5u;
    plant(&late.ce, 7101u, AE_CAND_ACTIVE, 4u, -30.0f);
    age_evidence(&late.ce, 7101u, poll0 - 8000000);
    EXPECT(promote_with_reserve(&late, 7101u, poll0));
    EXPECT(g_emit.starts == 1u);
    host_obs_poll(&late.om, poll0 + 10000000);
    EXPECT(g_emit.starts == 2u);
    const int li = find_bind_idx(&late, 7101u);
    EXPECT(li >= 0);
    EXPECT(late.om.bind[li].start_last_attempt_mono_us == poll0 + 10000000);
    EXPECT(late.om.bind[li].start_retry_not_before_mono_us == poll0 + 11000000);
    EXPECT(late.om.bind[li].start_capture_mono_us == poll0 - 8000000);
    fixture_fini(&late);
}

static void test_p7_prepare_reuses_after_failed_publish(void)
{
    ear_obs_start_retain_reset();
    fixture_t f;
    fixture_init(&f, false);
    g_use_start_prepare = true;
    g_ring_rms = -22.5f;
    g_emit.fail_start_remaining = 1u;
    plant(&f.ce, 7110u, AE_CAND_ACTIVE, 4u, -30.0f);
    const int64_t t0 = 7110LL * 1000;
    EXPECT(promote_with_reserve(&f, 7110u, t0));
    EXPECT(g_prepare_builds == 1);
    EXPECT(g_prepared.l0.rms_mean_dbfs == -22.5f);
    EXPECT(g_prepared.have_l1);
    EXPECT(g_prepared.have_l2);
    EXPECT(g_prepared.l2.window_count == 2u);
    EXPECT(g_prepared.capture_mono_us == t0);
    EXPECT(g_prepared.interval_start_mono_us == t0);
    EXPECT(strcmp(g_emit.sample[0].assess, "AIRCRAFT") == 0);
    const int bi = find_bind_idx(&f, 7110u);
    EXPECT(bi >= 0);
    g_ring_rms = 9.0f;
    f.om.bind[bi].cls_wire.present = true;
    f.om.bind[bi].cls_wire.n_nodes = 1u;
    snprintf(f.om.bind[bi].cls_wire.nodes[0].id, EAR_CLS_NODE_ID_MAX, "SIREN");
    host_obs_poll(&f.om, t0 + 1000000);
    EXPECT(g_emit.starts == 2u);
    EXPECT(g_prepare_builds == 1);
    EXPECT(g_prepared.l0.rms_mean_dbfs == -22.5f);
    EXPECT(g_prepared.l1.interval_start_mono_us == t0);
    EXPECT(g_prepared.capture_mono_us == t0);
    EXPECT(strcmp(g_emit.sample[1].assess, "AIRCRAFT") == 0);
    EXPECT(g_emit.sample[1].capture == t0);
    fixture_fini(&f);
}

static void test_p7_accepted_start_failed_end_still_releases(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 7060u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 7060u, 1000));
    EXPECT(g_emit.starts == 1u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 1u);
    g_emit.fail_end = true;
    ear_obs_manager_on_activity_end_hint(
        &f.om, EAR_OBS_ACT_END_AMBIENT, 2000, 2100, true, -50.0f
    );
    EXPECT(g_emit.ends == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);
    host_obs_poll(&f.om, 10000000);
    EXPECT(g_emit.starts == 1u);
    EXPECT(g_emit.ends == 1u);
    fixture_fini(&f);
}

static int64_t pending_promote_us(const fixture_t *f, uint64_t cid)
{
    for (uint8_t i = 0; i < EAR_OBS_MGR_PENDING_CAP; i++) {
        if (f->om.pending[i].in_use && !f->om.pending[i].cancelled
            && f->om.pending[i].candidate_id == cid) {
            return f->om.pending[i].snap.promote_mono_us;
        }
    }
    return -1;
}

/**
 * Aged activity END, then FIFO handoff. Capture stays the END anchor.
 * Attempt time is the pinned monotonic clock, not the END report and not
 * the queued promotion snapshot.
 */
static void test_p7_fifo_end_anchor_and_attempt_clock(void)
{
    fixture_t f;
    fixture_init(&f, false);
    g_emit.fail_start_remaining = 8u;
    g_mono_pinned = true;
    g_mono_now = 25000000;
    plant(&f.ce, 7201u, AE_CAND_ACTIVE, 4u, -30.0f);
    plant(&f.ce, 7202u, AE_CAND_ACTIVE, 4u, -31.0f);
    plant(&f.ce, 7203u, AE_CAND_ACTIVE, 4u, -32.0f);
    age_evidence(&f.ce, 7203u, 1000000);
    EXPECT(promote_with_reserve(&f, 7201u, 7201000));
    EXPECT(promote_with_reserve(&f, 7202u, 7202000));
    EXPECT(promote_with_reserve(&f, 7203u, 1000000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);
    EXPECT(ear_obs_manager_n_pending(&f.om) == 1u);
    const int direct = find_bind_idx(&f, 7201u);
    EXPECT(direct >= 0);
    EXPECT(f.om.bind[direct].start_mono_us == 7201LL * 1000);
    EXPECT(f.om.bind[direct].start_capture_mono_us == 7201LL * 1000);
    EXPECT(f.om.bind[direct].start_last_attempt_mono_us == 25000000);
    const int64_t promo = pending_promote_us(&f, 7203u);
    EXPECT(promo == 1000000);
    const int64_t handoff = 9000000;
    EXPECT(promo != handoff);
    g_mono_now = 25000000;
    ear_obs_manager_on_activity_end_hint(
        &f.om, EAR_OBS_ACT_END_AMBIENT, handoff - 1000000, handoff, true, -50.0f
    );
    EXPECT(g_emit.starts == 3u);
    EXPECT(g_emit.ends == 0u);
    const int bi = find_bind_idx(&f, 7203u);
    EXPECT(bi >= 0);
    EXPECT(f.om.bind[bi].start_mono_us == handoff);
    EXPECT(f.om.bind[bi].start_capture_mono_us == handoff);
    EXPECT(f.om.bind[bi].start_capture_mono_us != promo);
    EXPECT(g_emit.sample[2].capture == handoff);
    EXPECT(g_emit.sample[2].capture != promo);
    const sp_episode_slot_t *slot = &f.reg.slots[f.om.bind[bi].ep.index];
    EXPECT(slot->episode_start_mono_us == handoff);
    EXPECT(slot->episode_start_mono_us != promo);
    EXPECT(slot->observation_interval_ms == 1000u);
    EXPECT(f.om.bind[bi].start_last_attempt_mono_us == 25000000);
    EXPECT(f.om.bind[bi].start_last_attempt_mono_us != handoff);
    EXPECT(f.om.bind[bi].start_retry_not_before_mono_us == 26000000);
    g_mono_pinned = false;
    host_obs_poll(&f.om, 25999999);
    EXPECT(g_emit.starts == 3u);
    host_obs_poll(&f.om, 26000000);
    EXPECT(g_emit.starts == 4u);
    EXPECT(f.om.bind[bi].start_capture_mono_us == handoff);
    EXPECT(g_emit.sample[3].capture == handoff);
    EXPECT(f.om.bind[bi].start_last_attempt_mono_us == 26000000);
    fixture_fini(&f);
}

/** Aged AS-002 absence window, then FIFO handoff. Same three-way split. */
static void test_p7_fifo_absence_anchor_and_attempt_clock(void)
{
    fixture_t f;
    fixture_init(&f, false);
    f.om.cfg.as002_signature_absence_hold_ms = 1000u;
    g_emit.fail_start_remaining = 8u;
    g_mono_pinned = true;
    g_mono_now = 30000000;
    plant(&f.ce, 7301u, AE_CAND_ACTIVE, 4u, -30.0f);
    plant(&f.ce, 7302u, AE_CAND_ACTIVE, 4u, -31.0f);
    plant(&f.ce, 7303u, AE_CAND_ACTIVE, 4u, -32.0f);
    age_evidence(&f.ce, 7303u, 1500000);
    EXPECT(promote_as002_with_reserve(&f, 7301u, 7301000));
    EXPECT(promote_with_reserve(&f, 7302u, 7302000));
    EXPECT(promote_with_reserve(&f, 7303u, 1500000));
    EXPECT(ear_obs_manager_n_pending(&f.om) == 1u);
    const int64_t promo = pending_promote_us(&f, 7303u);
    EXPECT(promo == 1500000);
    const int64_t handoff = 8000000;
    g_mono_now = 30000000;
    ear_obs_manager_on_as002_siren_window(
        &f.om, true, false, 1000u, handoff, true, -40.0f
    );
    EXPECT(g_emit.starts == 3u);
    EXPECT(g_emit.ends == 0u);
    const int bi = find_bind_idx(&f, 7303u);
    EXPECT(bi >= 0);
    EXPECT(f.om.bind[bi].start_mono_us == handoff);
    EXPECT(f.om.bind[bi].start_capture_mono_us == handoff);
    EXPECT(f.om.bind[bi].start_capture_mono_us != promo);
    EXPECT(g_emit.sample[2].capture == handoff);
    const sp_episode_slot_t *slot = &f.reg.slots[f.om.bind[bi].ep.index];
    EXPECT(slot->episode_start_mono_us == handoff);
    EXPECT(slot->observation_interval_ms == 1000u);
    EXPECT(f.om.bind[bi].start_last_attempt_mono_us == 30000000);
    EXPECT(f.om.bind[bi].start_last_attempt_mono_us != handoff);
    EXPECT(f.om.bind[bi].start_retry_not_before_mono_us == 31000000);
    fixture_fini(&f);
}

/**
 * Two due bindings in one poll. The first callback blocks past one second.
 * Each binding is attempted once. The second deadline follows its own sample.
 */
static void test_p7_two_due_bindings_blocking_callback(void)
{
    fixture_t f;
    fixture_init(&f, false);
    g_emit.fail_start_remaining = 10u;
    g_mono_pinned = true;
    g_mono_now = 40000000;
    plant(&f.ce, 7401u, AE_CAND_ACTIVE, 4u, -30.0f);
    plant(&f.ce, 7402u, AE_CAND_ACTIVE, 4u, -31.0f);
    EXPECT(promote_with_reserve(&f, 7401u, 7401000));
    EXPECT(promote_with_reserve(&f, 7402u, 7402000));
    EXPECT(g_emit.starts == 2u);
    const int a = find_bind_idx(&f, 7401u);
    const int b = find_bind_idx(&f, 7402u);
    EXPECT(a == 0 && b == 1);
    EXPECT(f.om.bind[a].start_last_attempt_mono_us == 40000000);
    EXPECT(f.om.bind[b].start_last_attempt_mono_us == 40000000);
    g_start_callback_consume_us = 1500000;
    g_mono_pinned = false;
    const int64_t retry_at = 41000000;
    host_obs_poll(&f.om, retry_at);
    EXPECT(g_emit.starts == 4u);
    EXPECT(f.om.bind[a].start_last_attempt_mono_us == retry_at);
    EXPECT(f.om.bind[a].start_retry_not_before_mono_us == retry_at + 1000000);
    EXPECT(f.om.bind[b].start_last_attempt_mono_us == retry_at + 1500000);
    EXPECT(f.om.bind[b].start_retry_not_before_mono_us == retry_at + 2500000);
    EXPECT(f.om.bind[a].start_capture_mono_us == 7401LL * 1000);
    EXPECT(f.om.bind[b].start_capture_mono_us == 7402LL * 1000);
    g_start_callback_consume_us = 0;
    host_obs_poll(&f.om, retry_at + 1000000);
    EXPECT(g_emit.starts == 5u);
    EXPECT(f.om.bind[b].start_last_attempt_mono_us == retry_at + 1500000);
    fixture_fini(&f);
}

/** One poll both admits and is already past the retry deadline. Still one START. */
static void test_p7_same_poll_admits_without_retry(void)
{
    fixture_t f;
    fixture_init(&f, false);
    g_emit.fail_start_remaining = 1u;
    g_start_callback_consume_us = 2000000;
    g_mono_pinned = true;
    g_mono_now = 50000000;
    plant(&f.ce, 7501u, AE_CAND_ACTIVE, 4u, -30.0f);
    const int64_t capture = 45000000;
    age_evidence(&f.ce, 7501u, capture);
    EXPECT(promote_with_reserve(&f, 7501u, 50000000));
    EXPECT(g_emit.starts == 1u);
    const int bi = find_bind_idx(&f, 7501u);
    EXPECT(bi >= 0);
    EXPECT(f.om.bind[bi].start_capture_mono_us == capture);
    EXPECT(f.om.bind[bi].start_last_attempt_mono_us == 50000000);
    EXPECT(f.om.bind[bi].start_retry_not_before_mono_us == 51000000);
    EXPECT(f.om.bind[bi].start_retry_not_before_mono_us != capture + 1000000);
    EXPECT(g_mono_now == 52000000);
    fixture_fini(&f);
}

/** A poll ten seconds late retries once and arms the next deadline from that sample. */
static void test_p7_late_poll_one_retry(void)
{
    fixture_t f;
    fixture_init(&f, false);
    g_emit.fail_start_remaining = 4u;
    g_mono_pinned = true;
    g_mono_now = 60000000;
    plant(&f.ce, 7601u, AE_CAND_ACTIVE, 4u, -30.0f);
    age_evidence(&f.ce, 7601u, 1000000);
    EXPECT(promote_with_reserve(&f, 7601u, 60000000));
    EXPECT(g_emit.starts == 1u);
    g_mono_pinned = false;
    host_obs_poll(&f.om, 70000000);
    EXPECT(g_emit.starts == 2u);
    const int bi = find_bind_idx(&f, 7601u);
    EXPECT(bi >= 0);
    EXPECT(f.om.bind[bi].start_last_attempt_mono_us == 70000000);
    EXPECT(f.om.bind[bi].start_retry_not_before_mono_us == 71000000);
    EXPECT(f.om.bind[bi].start_capture_mono_us == 1000000);
    host_obs_poll(&f.om, 70999999);
    EXPECT(g_emit.starts == 2u);
    fixture_fini(&f);
}

static fixture_t *g_lock_fx;
static int g_lock_depth;
static int g_lock_takes;
static int g_lock_gives;
static int g_lock_max_depth;
static bool g_accepted_at_first_give;
static bool g_ota_saw_accepted;
static sp_obs_emit_gate_t g_section_gate;

static bool registry_has_accepted_start(const fixture_t *f)
{
    for (uint8_t i = 0; i < SP_OBS_EPISODE_SLOTS; i++) {
        if (f->reg.slots[i].in_use && f->reg.slots[i].start_publish_accepted) {
            return true;
        }
    }
    return false;
}

static void lock_reset(fixture_t *f)
{
    g_lock_fx = f;
    g_lock_depth = 0;
    g_lock_takes = 0;
    g_lock_gives = 0;
    g_lock_max_depth = 0;
    g_accepted_at_first_give = false;
    g_ota_saw_accepted = false;
    g_section_gate = SP_OBS_GATE_RUN;
}

static void section_lock_take(void *ctx)
{
    (void)ctx;
    g_lock_takes++;
    g_lock_depth++;
    if (g_lock_depth > g_lock_max_depth) {
        g_lock_max_depth = g_lock_depth;
    }
    EXPECT(g_lock_depth == 1);
}

static void section_lock_give(void *ctx)
{
    (void)ctx;
    if (g_lock_gives == 0 && g_lock_fx != NULL) {
        g_accepted_at_first_give = registry_has_accepted_start(g_lock_fx);
    }
    EXPECT(g_lock_depth == 1);
    g_lock_gives++;
    g_lock_depth--;
}

static void ota_close_body(void *ctx)
{
    (void)ctx;
    if (g_lock_fx != NULL) {
        g_ota_saw_accepted = registry_has_accepted_start(g_lock_fx);
        ear_obs_manager_forget_bindings(&g_lock_fx->om);
    }
    g_section_gate = SP_OBS_GATE_CLOSED;
}

static bool queue_as001_without_poll(fixture_t *f, uint64_t cid)
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
    return true;
}

/**
 * Production section: ear_sig_product_poll_in_section runs a real Observation
 * Manager START. Acceptance is visible at the first give. OTA uses
 * ear_obs_lifecycle_section on the same lock.
 */
static void test_p7_product_section_start_before_unlock(void)
{
    ear_obs_lifecycle_lock_ops_t lock = {
        .take = section_lock_take,
        .give = section_lock_give,
        .lock_ctx = NULL,
    };

    fixture_t win;
    fixture_init(&win, true);
    lock_reset(&win);
    g_mono_now = 80000000;
    plant(&win.ce, 7701u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(queue_as001_without_poll(&win, 7701u));
    ear_sig_product_t prod = {
        .om = &win.om,
        .policy = &win.pol,
        .ce = &win.ce,
        .ready = true,
    };
    ear_sig_product_section_args_t args = {
        .prod = &prod,
        .now_mono_us = 80000000,
        .before_poll = NULL,
        .after_poll = NULL,
        .before_ctx = NULL,
    };
    EXPECT(
        ear_sig_product_poll_in_section(&lock, &g_section_gate, &args)
        == EAR_OBS_LIFECYCLE_SECTION_RAN
    );
    EXPECT(g_emit.starts == 1u);
    EXPECT(g_accepted_at_first_give);
    EXPECT(registry_has_accepted_start(&win));
    EXPECT(g_lock_takes == 1);
    EXPECT(g_lock_gives == 1);
    EXPECT(g_lock_depth == 0);
    EXPECT(g_lock_max_depth == 1);
    EXPECT(
        ear_obs_lifecycle_section(&lock, NULL, ota_close_body, NULL)
        == EAR_OBS_LIFECYCLE_SECTION_RAN
    );
    EXPECT(g_ota_saw_accepted);
    EXPECT(g_section_gate == SP_OBS_GATE_CLOSED);
    EXPECT(ear_obs_manager_n_open(&win.om) == 0u);
    const uint32_t starts_after_ota = g_emit.starts;
    EXPECT(
        ear_sig_product_poll_in_section(&lock, &g_section_gate, &args)
        == EAR_OBS_LIFECYCLE_SECTION_SKIPPED_GATE
    );
    EXPECT(g_emit.starts == starts_after_ota);
    EXPECT(g_lock_takes == g_lock_gives);
    EXPECT(g_lock_depth == 0);
    EXPECT(g_lock_max_depth == 1);
    fixture_fini(&win);

    fixture_t ota_first;
    fixture_init(&ota_first, true);
    lock_reset(&ota_first);
    g_mono_now = 81000000;
    plant(&ota_first.ce, 7702u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(queue_as001_without_poll(&ota_first, 7702u));
    ear_sig_product_t prod_b = {
        .om = &ota_first.om,
        .policy = &ota_first.pol,
        .ce = &ota_first.ce,
        .ready = true,
    };
    ear_sig_product_section_args_t args_b = {
        .prod = &prod_b,
        .now_mono_us = 81000000,
    };
    EXPECT(
        ear_obs_lifecycle_section(&lock, NULL, ota_close_body, NULL)
        == EAR_OBS_LIFECYCLE_SECTION_RAN
    );
    EXPECT(!g_ota_saw_accepted);
    EXPECT(g_section_gate == SP_OBS_GATE_CLOSED);
    EXPECT(
        ear_sig_product_poll_in_section(&lock, &g_section_gate, &args_b)
        == EAR_OBS_LIFECYCLE_SECTION_SKIPPED_GATE
    );
    EXPECT(g_emit.starts == 0u);
    EXPECT(!registry_has_accepted_start(&ota_first));
    EXPECT(g_lock_takes == g_lock_gives);
    EXPECT(g_lock_depth == 0);
    EXPECT(g_lock_max_depth == 1);
    fixture_fini(&ota_first);
}

int main(void)
{
    test_no_promote_no_obs();
    test_reserve_full_blocks_promote();
    test_promote_fail_releases_rsv();
    test_enter_one_obs();
    test_dup_enter();
    test_capacity_pending();
    test_held_counts();
    test_no_overbook_and_dup_release();
    test_engine_stop();
    test_fifo_drain_order();
    test_fifo_before_new_promote();
    test_fifo_no_skip_on_alloc_fail();
    test_pending_cancel_advances();
    test_poll_recovers_fifo();
    test_exit_detach_no_end();
    test_pending_timeout_and_sync_exit();
    test_end_once();
    test_end_order_a_end_before_exit();
    test_end_order_b_exit_before_end();
    test_exit_alone_no_end();
    test_periodic_hint_reporting_fields();
    test_two_open_mixed_ce_live_shared_end();
    test_two_open_shared_periodic_duplicated();
    test_two_open_both_live_end_latches_both();
    test_rematch_after_end_no_revive();
    test_new_promote_after_end_stale_exit_safe();
    test_capacity_recovery_after_end();
    test_end_without_exit_unsticks_bindings();
    test_evidence_owned();
    test_s2_post_promote_adds_sig_copies();
    test_s2_two_candidates_no_cross_contaminate();
    test_s2_ae_history_overwrite_safe();
    test_s2_gap_invalid_rejected();
    test_s2_dup_and_out_of_order_seq();
    test_s2_stale_promotion_gen_dropped();
    test_s2_enter_snapshot_unchanged();
    test_s2_rejects_unassociated_ae_latest();
    test_s3_detached_ends_after_timeout();
    test_s3_idle_rms_path_ends_after_detach_timeout();
    test_s3_reattach_cancels_timer();
    test_s3_two_obs_only_detached_ends();
    test_s3_ambient_end_still_immediate();
    test_s3_exit_alone_no_immediate_end();
    test_s3_as002_excluded_from_watchdog();
    test_s3_stale_gen_no_timer();
    test_s3_reused_binding_no_inherited_detach();
    test_s3_timeout_boundary_monotonic();
    test_s3_periodic_does_not_touch_detached_timer();
    test_s3_engine_stop_then_watchdog_end();
    test_policy_reserve_gates();
    test_orphan_enter();
    test_ce_exit_payload_gen_propagates_stale_safe();
    test_zero_gen_exit_rejected_on_live_binding();
    test_end_metadata_snapshot();
    test_end_publish_fail_still_releases();
    test_end_reentrant_exactly_once();
    test_p2_ambient_active_no_obs();
    test_p2_as001_can_promote();
    test_p2_loud_unmatched_not_promoted();
    test_p2_held_irrelevant_consumes_no_capacity();
    test_p2_pending_entries_are_approved();
    test_p2_pending_drain_rejects_unapproved();
    test_p2_same_signature_one_observation();
    test_p2_no_repeat_while_promoted();
    test_p2_commit_rejects_unapproved_snapshot();
    test_p2_classification_cleared_before_enter();
    test_p2_stale_as001_across_gap();
    test_p2_promote_with_reserve_bypasses_policy();
    test_p2_enter_queue_full_recovers();
    test_p2_unadmit_gen_protects_newer();
    test_p2_as001_ce_om_lifecycle_e2e();
    test_p3b_fifo_handoff_rejects_old_generation();
    test_p7_start_retry_same_identity();
    test_p7_assessment_frozen_across_retry();
    test_p7_retry_spacing_and_no_duplicate();
    test_p7_periodic_suppressed_both_paths();
    test_p7_silent_end_before_accept();
    test_p7_exit_and_watchdog_before_accept();
    test_p7_as002_absence_and_invalid_hold_before_accept();
    test_p7_two_slots_independent();
    test_p7_stale_handle_and_fifo();
    test_p7_ota_and_lock_and_corrupt_retry();
    test_p7_measure_retain_bytes();
    test_p7_attempt_clock();
    test_p7_prepare_reuses_after_failed_publish();
    test_p7_accepted_start_failed_end_still_releases();
    test_p7_fifo_end_anchor_and_attempt_clock();
    test_p7_fifo_absence_anchor_and_attempt_clock();
    test_p7_two_due_bindings_blocking_callback();
    test_p7_same_poll_admits_without_retry();
    test_p7_late_poll_one_retry();
    test_p7_product_section_start_before_unlock();

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("ok — ear_obs_manager contract tests passed\n");
    return 0;
}
