/**
 * Host tests: EAR Observation Manager promotion-gated contract (v1).
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
 *     firmware/products/dbmap/ear/ear_observation/test_host/test_ear_obs_manager.c \
 *     -lm -o /tmp/test_ear_obs_manager && /tmp/test_ear_obs_manager
 */
#include "ae_engine.h"
#include "ae_signature.h"
#include "ce_engine.h"
#include "ear_obs_manager.h"
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

typedef struct {
    uint32_t starts;
    uint32_t periodics;
    uint32_t ends;
    uint64_t last_start_cand;
    char last_obs_id[SP_OBS_EPISODE_ID_LEN];
    int64_t last_periodic_start;
    int64_t last_periodic_end;
    bool last_periodic_has_lvl;
    float last_periodic_lvl;
    int64_t last_end_start;
    int64_t last_end_end;
    bool last_end_has_lvl;
    float last_end_lvl;
    ear_obs_end_reason_t last_end_reason;
} emit_counts_t;

static emit_counts_t g_emit;

static bool emit_start(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    const ear_obs_promote_snapshot_t *snap,
    int64_t now_mono_us
)
{
    (void)ctx;
    (void)snap;
    (void)now_mono_us;
    g_emit.starts++;
    g_emit.last_start_cand = binding->candidate_id;
    memcpy(g_emit.last_obs_id, binding->observation_id, sizeof(g_emit.last_obs_id));
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
    (void)binding;
    g_emit.periodics++;
    g_emit.last_periodic_start = interval_start_mono_us;
    g_emit.last_periodic_end = interval_end_mono_us;
    g_emit.last_periodic_has_lvl = has_signal_level_dbfs;
    g_emit.last_periodic_lvl = signal_level_dbfs;
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
    (void)ctx;
    (void)binding;
    g_emit.ends++;
    g_emit.last_end_reason = reason;
    g_emit.last_end_start = interval_start_mono_us;
    g_emit.last_end_end = interval_end_mono_us;
    g_emit.last_end_has_lvl = has_signal_level_dbfs;
    g_emit.last_end_lvl = signal_level_dbfs;
    return true;
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

static void fixture_init(fixture_t *f, bool bind_policy)
{
    memset(f, 0, sizeof(*f));
    memset(&g_emit, 0, sizeof(g_emit));
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
    mcfg.emit_start = emit_start;
    mcfg.emit_periodic = emit_periodic;
    mcfg.emit_end = emit_end;
    mcfg.enable_poll_periodic = true;
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

static bool promote_with_reserve(fixture_t *f, uint64_t cid, int64_t now_us)
{
    ear_obs_rsv_handle_t h;
    if (ear_obs_manager_reserve(&f->om, &h) != EAR_OBS_RSV_OK) {
        return false;
    }
    if (!ear_obs_manager_bind_reservation_candidate(&f->om, h, cid)) {
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

/* 1. Candidate not promoted → no Observation */
static void test_no_promote_no_obs(void)
{
    fixture_t f;
    fixture_init(&f, true);
    plant(&f.ce, 10u, AE_CAND_ACTIVE, 5u, -40.0f);
    ear_obs_manager_poll(&f.om, 1000);
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
    ear_obs_manager_poll(&f.om, 6000);
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
    EXPECT(ear_obs_manager_commit_reservation(&f.om, junk, 99u, NULL) == false);
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
    ear_obs_manager_poll(&f.om, 9000); /* drain EXIT */
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

    /* End only candidate 50's binding. */
    for (uint8_t i = 0; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == 50u) {
            /* Mark ce_live false and use end hint path for one. */
            f.om.bind[i].ce_live = false;
        }
    }
    ear_obs_manager_on_activity_end_hint(&f.om, 3000, 3500, true, -40.0f);
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
    ear_obs_manager_poll(&f.om, 5000);
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
    ear_obs_manager_poll(&f.om, 4500);
    EXPECT(ear_obs_manager_n_pending(&f.om) == 1u);
    EXPECT(f.om.pending[f.om.pending_order[0]].candidate_id == 73u);

    /* Free a slot → 73 advances */
    for (uint8_t i = 0; i < 2; i++) {
        if (f.om.bind[i].in_use && f.om.bind[i].candidate_id == 70u) {
            f.om.bind[i].ce_live = false;
        }
    }
    ear_obs_manager_on_activity_end_hint(&f.om, 4500, 5000, true, -40.0f);
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
    ear_obs_manager_poll(&f.om, 4000); /* recovery drain */
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
    ear_obs_manager_poll(&f.om, 2000);
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
    ear_obs_manager_poll(&f.om, 2000000); /* 2s > 1s timeout */
    EXPECT(ear_obs_manager_n_pending(&f.om) == 0u);
    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.pending_timeout >= 1u);

    /* Duplicate timeout/poll harmless */
    ear_obs_manager_poll(&f.om, 3000000);
    ear_obs_manager_get_diag(&f.om, &d);
    const uint32_t t1 = d.pending_timeout;
    ear_obs_manager_poll(&f.om, 4000000);
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.pending_timeout == t1);

    /* Duplicate EXIT */
    ae_candidate_t c;
    memset(&c, 0, sizeof(c));
    c.candidate_id = 102u;
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &f.om);
    ear_obs_manager_poll(&f.om, 5000000);
    EXPECT(ear_obs_manager_n_pending(&f.om) == 0u);
    fixture_fini(&f);
}

/* 23. END + registry release exactly once */
static void test_end_once(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 110u, AE_CAND_ACTIVE, 3u, -30.0f);
    EXPECT(promote_with_reserve(&f, 110u, 1000));
    for (uint8_t i = 0; i < 2; i++) {
        if (f.om.bind[i].in_use) {
            f.om.bind[i].ce_live = false;
        }
    }
    ear_obs_manager_on_activity_end_hint(&f.om, 1500, 2000, true, -38.0f);
    EXPECT(g_emit.ends == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);
    ear_obs_manager_on_activity_end_hint(&f.om, 2500, 3000, true, -38.0f);
    EXPECT(g_emit.ends == 1u);
    fixture_fini(&f);
}

/* Order A: RMS END before CE EXIT → latch → END on EXIT */
static void test_end_order_a_end_before_exit(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 400u, AE_CAND_ACTIVE, 4u, -30.0f);
    EXPECT(promote_with_reserve(&f, 400u, 1000));
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(ear_obs_manager_binding_ce_live(&f.om, 400u));

    ear_obs_manager_on_activity_end_hint(&f.om, 1800, 2000, true, -45.0f);
    EXPECT(ear_obs_manager_binding_end_hint_latched(&f.om, 400u));
    EXPECT(g_emit.ends == 0u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 1u);

    /* Duplicate END while latched: still one latch, no END yet. */
    ear_obs_manager_on_activity_end_hint(&f.om, 1900, 2100, true, -46.0f);
    EXPECT(ear_obs_manager_binding_end_hint_latched(&f.om, 400u));
    EXPECT(g_emit.ends == 0u);

    EXPECT(ce_reject(&f.ce, 400u));
    ear_obs_manager_poll(&f.om, 2200);
    EXPECT(g_emit.ends == 1u);
    EXPECT(g_emit.last_end_reason == EAR_OBS_END_HINT_ACTIVITY);
    EXPECT(g_emit.last_end_start == 1900); /* last latched interval */
    EXPECT(g_emit.last_end_end == 2100);
    EXPECT(g_emit.last_end_has_lvl);
    EXPECT(g_emit.last_end_lvl == -46.0f);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);

    /* Duplicate EXIT after END: harmless */
    ae_candidate_t c;
    memset(&c, 0, sizeof(c));
    c.candidate_id = 400u;
    ear_obs_manager_on_promotion(&c, CE_PROMOTION_EXIT, &f.om);
    ear_obs_manager_poll(&f.om, 2300);
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
    ear_obs_manager_poll(&f.om, 1500);
    EXPECT(!ear_obs_manager_binding_ce_live(&f.om, 401u));
    EXPECT(g_emit.ends == 0u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);

    ear_obs_manager_on_activity_end_hint(&f.om, 1600, 1700, true, -44.0f);
    EXPECT(g_emit.ends == 1u);
    EXPECT(g_emit.last_end_start == 1600);
    EXPECT(g_emit.last_end_end == 1700);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);

    ear_obs_manager_on_activity_end_hint(&f.om, 1800, 1900, true, -44.0f);
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
    ear_obs_manager_poll(&f.om, 1500);
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
 * Two open Obs, mixed ce_live, one global RMS END:
 * detached → END now; live → latch only (EXIT still required to consume).
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
    ear_obs_manager_poll(&f.om, 2500);
    EXPECT(!ear_obs_manager_binding_ce_live(&f.om, 420u));
    EXPECT(ear_obs_manager_binding_ce_live(&f.om, 421u));
    EXPECT(g_emit.ends == 0u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);

    /* Global RMS END: ends 420, latches 421. */
    ear_obs_manager_on_activity_end_hint(&f.om, 2600, 2700, true, -48.0f);
    EXPECT(g_emit.ends == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(ear_obs_manager_binding_ce_live(&f.om, 421u));
    EXPECT(ear_obs_manager_binding_end_hint_latched(&f.om, 421u));
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 1u);

    /* 421 still open until its EXIT consumes the latch. */
    EXPECT(ce_reject(&f.ce, 421u));
    ear_obs_manager_poll(&f.om, 2800);
    EXPECT(g_emit.ends == 2u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(sp_episode_registry_in_use_count(&f.reg) == 0u);
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

/** Both live: one END latches both; neither ends until each EXIT. */
static void test_two_open_both_live_end_latches_both(void)
{
    fixture_t f;
    fixture_init(&f, false);
    plant(&f.ce, 440u, AE_CAND_ACTIVE, 5u, -30.0f);
    plant(&f.ce, 441u, AE_CAND_ACTIVE, 4u, -31.0f);
    EXPECT(promote_with_reserve(&f, 440u, 1000));
    EXPECT(promote_with_reserve(&f, 441u, 2000));

    ear_obs_manager_on_activity_end_hint(&f.om, 3000, 3100, true, -50.0f);
    EXPECT(g_emit.ends == 0u);
    EXPECT(ear_obs_manager_binding_end_hint_latched(&f.om, 440u));
    EXPECT(ear_obs_manager_binding_end_hint_latched(&f.om, 441u));
    EXPECT(ear_obs_manager_n_open(&f.om) == 2u);

    EXPECT(ce_reject(&f.ce, 440u));
    ear_obs_manager_poll(&f.om, 3200);
    EXPECT(g_emit.ends == 1u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 1u);
    EXPECT(ear_obs_manager_binding_end_hint_latched(&f.om, 441u));

    EXPECT(ce_reject(&f.ce, 441u));
    ear_obs_manager_poll(&f.om, 3300);
    EXPECT(g_emit.ends == 2u);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
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
    ear_obs_manager_poll(&f.om, 6000);

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

/* Policy: reserve-full prevents promote */
static void test_policy_reserve_gates(void)
{
    fixture_t f;
    fixture_init(&f, true);
    plant(&f.ce, 300u, AE_CAND_ACTIVE, 5u, -30.0f);
    plant(&f.ce, 301u, AE_CAND_ACTIVE, 4u, -31.0f);
    plant(&f.ce, 302u, AE_CAND_ACTIVE, 3u, -32.0f);
    plant(&f.ce, 303u, AE_CAND_ACTIVE, 2u, -33.0f);

    for (int i = 0; i < 6; i++) {
        ear_promotion_policy_poll(&f.pol, (int64_t)(i + 1) * 1000);
        ear_obs_manager_poll(&f.om, (int64_t)(i + 1) * 1000);
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
    ear_obs_manager_poll(&f.om, 1000);
    EXPECT(ear_obs_manager_n_open(&f.om) == 0u);
    EXPECT(g_emit.starts == 0u);
    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&f.om, &d);
    EXPECT(d.enter_no_rsv >= 1u);
    fixture_fini(&f);
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
    test_evidence_owned();
    test_policy_reserve_gates();
    test_orphan_enter();

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("ok — ear_obs_manager contract tests passed\n");
    return 0;
}
