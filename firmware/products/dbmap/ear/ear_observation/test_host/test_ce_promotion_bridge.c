/**
 * Host tests: M3 CE promotion bridge (association-only).
 *
 * Covers D4 ranking key isolation, eligibility, identity/lifecycle, Observation
 * authority, and defensive queue overflow (artificial host saturation — not the
 * device single-task model; see ce_promotion_bridge.h invariant comment).
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
 *     firmware/products/dbmap/ear/audio_engine/ae_engine.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_engine.c \
 *     firmware/platform/observation/episode_registry.c \
 *     firmware/products/dbmap/ear/ear_observation/ce_promotion_bridge.c \
 *     firmware/products/dbmap/ear/ear_observation/test_host/test_ce_promotion_bridge.c \
 *     -lm -o /tmp/test_ce_bridge && /tmp/test_ce_bridge
 */
#include "ae_engine.h"
#include "ae_signature.h"
#include "ce_engine.h"
#include "ce_promotion_bridge.h"
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

static void publish_tone(ae_engine_t *ae, uint32_t seq, float rms, bool gap)
{
    dbmap_l0_window_t l0;
    fill_l0(&l0, rms, (int64_t)seq * 1000000);
    ae_signature_t sig;
    EXPECT(ae_signature_build(seq, &l0, NULL, NULL, NULL, gap, &sig));
    EXPECT(ae_publish_signature(ae, &sig));
}

static void fake_uuid_a(char out[SP_OBS_EPISODE_ID_LEN])
{
    memcpy(out, "11111111-1111-1111-1111-111111111111", 36);
    out[36] = '\0';
}

static void fake_uuid_b(char out[SP_OBS_EPISODE_ID_LEN])
{
    memcpy(out, "22222222-2222-2222-2222-222222222222", 36);
    out[36] = '\0';
}

static void ce_engine_ready(ce_engine_t *ce)
{
    ce_config_t ccfg;
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 2u;
    EXPECT(ce_start(ce, &ccfg));
}

/**
 * Test-only fixture: plant an in-use candidate with controlled ranking fields.
 * Isolates D4 keys without depending on fingerprint EMA evolution.
 */
static ae_candidate_t *plant_candidate(
    ce_engine_t *ce,
    uint64_t id,
    ae_cand_state_t state,
    uint32_t evidence,
    float ema_rms,
    uint8_t quality
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
        ce->slots[i].ema_rms_dbfs = ema_rms;
        ce->slots[i].quality_summary = quality;
        if (id >= ce->next_id) {
            ce->next_id = id + 1u;
        }
        return &ce->slots[i];
    }
    EXPECT(0 && "no free CE slot");
    return NULL;
}

static void open_obs(
    ce_promotion_bridge_t *bridge,
    sp_episode_registry_t *reg,
    sp_ep_handle_t *out_h,
    sp_ep_uuid_fn uuid_fn
)
{
    EXPECT(sp_episode_registry_allocate(reg, out_h, 1000, 5000, 1u, uuid_fn, false));
    const sp_episode_slot_t *slot = sp_episode_registry_get_const(reg, *out_h);
    EXPECT(slot != NULL);
    ce_bridge_notify_obs_open(bridge, slot->observation_id, *out_h);
}

/* -------------------------------------------------------------------------- */
/* D4 ranking — one deciding key per test                                     */
/* -------------------------------------------------------------------------- */

static void test_rank_by_evidence_count(void)
{
    ce_engine_t ce;
    ce_engine_ready(&ce);
    /* Equal ema + quality; only evidence differs. */
    plant_candidate(&ce, 10u, AE_CAND_ACTIVE, 3u, -40.0f, AE_QUALITY_OK);
    plant_candidate(&ce, 11u, AE_CAND_ACTIVE, 7u, -40.0f, AE_QUALITY_OK);

    uint64_t primary = 0u;
    EXPECT(ce_bridge_select_primary(&ce, &primary));
    EXPECT(primary == 11u);
}

static void test_rank_by_ema_rms_tiebreak(void)
{
    ce_engine_t ce;
    ce_engine_ready(&ce);
    /* Equal evidence + quality; only ema differs. */
    plant_candidate(&ce, 20u, AE_CAND_ACTIVE, 5u, -55.0f, AE_QUALITY_OK);
    plant_candidate(&ce, 21u, AE_CAND_ACTIVE, 5u, -35.0f, AE_QUALITY_OK);

    uint64_t primary = 0u;
    EXPECT(ce_bridge_select_primary(&ce, &primary));
    EXPECT(primary == 21u);
}

static void test_rank_by_lower_candidate_id_tiebreak(void)
{
    ce_engine_t ce;
    ce_engine_ready(&ce);
    /* Equal evidence + equal ema + quality; only id differs. */
    plant_candidate(&ce, 31u, AE_CAND_ACTIVE, 4u, -42.0f, AE_QUALITY_DEGRADED);
    plant_candidate(&ce, 30u, AE_CAND_ACTIVE, 4u, -42.0f, AE_QUALITY_DEGRADED);

    uint64_t primary = 0u;
    EXPECT(ce_bridge_select_primary(&ce, &primary));
    EXPECT(primary == 30u);

    uint64_t again = 0u;
    EXPECT(ce_bridge_select_primary(&ce, &again));
    EXPECT(again == 30u);
}

/* -------------------------------------------------------------------------- */
/* Eligibility                                                                */
/* -------------------------------------------------------------------------- */

static void test_eligibility_nascent_not_selected(void)
{
    ce_engine_t ce;
    ce_engine_ready(&ce);
    plant_candidate(&ce, 40u, AE_CAND_NASCENT, 9u, -20.0f, AE_QUALITY_OK);
    uint64_t primary = 0u;
    EXPECT(!ce_bridge_select_primary(&ce, &primary));
}

static void test_eligibility_coasting_not_selected(void)
{
    ce_engine_t ce;
    ce_engine_ready(&ce);
    plant_candidate(&ce, 41u, AE_CAND_COASTING, 9u, -20.0f, AE_QUALITY_OK);
    uint64_t primary = 0u;
    EXPECT(!ce_bridge_select_primary(&ce, &primary));
}

static void test_eligibility_promoted_blocks_new_primary(void)
{
    ce_engine_t ce;
    ce_engine_ready(&ce);
    plant_candidate(&ce, 42u, AE_CAND_PROMOTED, 2u, -50.0f, AE_QUALITY_OK);
    plant_candidate(&ce, 43u, AE_CAND_ACTIVE, 99u, -10.0f, AE_QUALITY_OK);
    uint64_t primary = 0u;
    EXPECT(!ce_bridge_select_primary(&ce, &primary));
}

/** COASTING with promotion_entered (post-ENTER gap) also blocks a new primary. */
static void test_eligibility_promotion_entered_coasting_blocks(void)
{
    ce_engine_t ce;
    ce_engine_ready(&ce);
    ae_candidate_t *a =
        plant_candidate(&ce, 44u, AE_CAND_COASTING, 2u, -50.0f, AE_QUALITY_OK);
    EXPECT(a != NULL);
    a->promotion_entered = true;
    plant_candidate(&ce, 45u, AE_CAND_ACTIVE, 99u, -10.0f, AE_QUALITY_OK);
    uint64_t primary = 0u;
    EXPECT(!ce_bridge_select_primary(&ce, &primary));
}

static void test_eligibility_quality_ok_and_degraded_accepted(void)
{
    ce_engine_t ce;
    ce_engine_ready(&ce);
    plant_candidate(&ce, 50u, AE_CAND_ACTIVE, 2u, -40.0f, AE_QUALITY_OK);
    uint64_t primary = 0u;
    EXPECT(ce_bridge_select_primary(&ce, &primary));
    EXPECT(primary == 50u);

    ce_engine_ready(&ce);
    plant_candidate(&ce, 51u, AE_CAND_ACTIVE, 2u, -40.0f, AE_QUALITY_DEGRADED);
    EXPECT(ce_bridge_select_primary(&ce, &primary));
    EXPECT(primary == 51u);
}

static void test_eligibility_unsupported_quality_rejected(void)
{
    ce_engine_t ce;
    ce_engine_ready(&ce);
    plant_candidate(&ce, 52u, AE_CAND_ACTIVE, 8u, -20.0f, AE_QUALITY_GAP);
    plant_candidate(&ce, 53u, AE_CAND_ACTIVE, 8u, -20.0f, AE_QUALITY_INVALID);
    uint64_t primary = 0u;
    EXPECT(!ce_bridge_select_primary(&ce, &primary));
}

static void test_eligibility_no_promote_without_obs_context(void)
{
    ce_engine_t ce;
    ce_promotion_bridge_t bridge;
    ce_engine_ready(&ce);
    plant_candidate(&ce, 60u, AE_CAND_ACTIVE, 5u, -40.0f, AE_QUALITY_OK);
    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);

    ce_bridge_poll(&bridge, NULL);
    EXPECT(!ce_bridge_is_bound(&bridge));
    ae_candidate_t c;
    EXPECT(ce_find_candidate(&ce, 60u, &c));
    EXPECT(c.state == AE_CAND_ACTIVE); /* not promoted without obs */
    ce_bridge_diag_t d;
    ce_bridge_get_diag(&bridge, &d);
    EXPECT(d.assoc_success == 0u);
    EXPECT(d.promote_calls == 0u);
}

static void test_eligibility_no_second_promote_while_promoted(void)
{
    ce_engine_t ce;
    ce_promotion_bridge_t bridge;
    sp_episode_registry_t reg;
    sp_ep_handle_t h;

    ce_engine_ready(&ce);
    plant_candidate(&ce, 70u, AE_CAND_ACTIVE, 5u, -30.0f, AE_QUALITY_OK);
    plant_candidate(&ce, 71u, AE_CAND_ACTIVE, 4u, -30.0f, AE_QUALITY_OK);
    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);
    sp_episode_registry_init(&reg);
    open_obs(&bridge, &reg, &h, fake_uuid_a);

    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 70u);

    ce_bridge_poll(&bridge, &reg);
    ae_candidate_t c71;
    EXPECT(ce_find_candidate(&ce, 71u, &c71));
    EXPECT(c71.state == AE_CAND_ACTIVE);
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 70u);
}

/* -------------------------------------------------------------------------- */
/* Identity / lifecycle                                                       */
/* -------------------------------------------------------------------------- */

static void feed_ce_gap(ce_engine_t *ce, uint32_t seq)
{
    dbmap_l0_window_t l0;
    fill_l0(&l0, -40.0f, (int64_t)seq * 1000000);
    ae_signature_t sig;
    EXPECT(ae_signature_build(seq, &l0, NULL, NULL, NULL, true, &sig));
    ce_engine_on_signature(ce, &sig);
}

static void test_candidate_slot_reuse_no_identity_confusion(void)
{
    ce_engine_t ce;
    ce_promotion_bridge_t bridge;
    sp_episode_registry_t reg;
    sp_ep_handle_t h;

    ce_config_t ccfg;
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 2u;
    ccfg.t_coast = 1u;
    EXPECT(ce_start(&ce, &ccfg));

    ae_candidate_t *slot0 =
        plant_candidate(&ce, 200u, AE_CAND_ACTIVE, 3u, -40.0f, AE_QUALITY_OK);
    EXPECT(slot0 != NULL);
    const uint64_t old_id = 200u;
    const uint8_t reused_index = (uint8_t)(slot0 - ce.slots);

    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);
    sp_episode_registry_init(&reg);
    open_obs(&bridge, &reg, &h, fake_uuid_a);

    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == old_id);
    EXPECT(ce.slots[reused_index].state == AE_CAND_PROMOTED);
    EXPECT(ce.slots[reused_index].promotion_entered);

    /* Real CE path: PROMOTED → COASTING → EXPIRED emits EXIT (no injection). */
    feed_ce_gap(&ce, 1u);
    EXPECT(ce.slots[reused_index].state == AE_CAND_COASTING);
    EXPECT(ce.slots[reused_index].promotion_entered);
    feed_ce_gap(&ce, 2u);
    EXPECT(!ce.slots[reused_index].in_use);
    EXPECT(!ce.slots[reused_index].promotion_entered);

    ce_bridge_poll(&bridge, &reg);
    EXPECT(!ce_bridge_is_bound(&bridge));

    /* Same array slot, new candidate identity — must not inherit promotion. */
    ae_candidate_t *slot_again = plant_candidate(
        &ce, 201u, AE_CAND_ACTIVE, 3u, -40.0f, AE_QUALITY_OK
    );
    EXPECT(slot_again == &ce.slots[reused_index]);
    EXPECT(slot_again->candidate_id == 201u);
    EXPECT(slot_again->candidate_id != old_id);
    EXPECT(!slot_again->promotion_entered);

    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 201u);
    EXPECT(ce_bridge_bound_candidate_id(&bridge) != old_id);
}

static void test_episode_generation_reuse_cannot_affect_new_assoc(void)
{
    ce_engine_t ce;
    ce_promotion_bridge_t bridge;
    sp_episode_registry_t reg;
    sp_ep_handle_t h1;
    sp_ep_handle_t h2;

    ce_engine_ready(&ce);
    plant_candidate(&ce, 80u, AE_CAND_ACTIVE, 3u, -40.0f, AE_QUALITY_OK);
    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);
    sp_episode_registry_init(&reg);

    open_obs(&bridge, &reg, &h1, fake_uuid_a);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 80u);
    const uint16_t gen1 = h1.generation;
    const uint8_t idx1 = h1.index;

    ce_bridge_diag_t d_enter;
    ce_bridge_get_diag(&bridge, &d_enter);
    EXPECT(d_enter.promote_calls == 1u);
    EXPECT(d_enter.enter_queued == 1u);

    ce_bridge_notify_obs_closed(&bridge);
    sp_episode_registry_release(&reg, h1);
    EXPECT(!ce_bridge_is_bound(&bridge));
    EXPECT(!sp_episode_registry_handle_valid(&reg, h1));

    /* Promotion lifecycle survives Observation close (Model A). */
    ae_candidate_t still;
    EXPECT(ce_find_candidate(&ce, 80u, &still));
    EXPECT(still.state == AE_CAND_PROMOTED);
    EXPECT(still.promotion_entered);

    /* Re-allocate — typically same slot index, new generation. */
    EXPECT(sp_episode_registry_allocate(&reg, &h2, 2000, 5000, 1u, fake_uuid_b, false));
    EXPECT(h2.index == idx1);
    EXPECT(h2.generation != gen1);
    EXPECT(!sp_episode_registry_handle_valid(&reg, h1));

    const sp_episode_slot_t *slot2 = sp_episode_registry_get_const(&reg, h2);
    ce_bridge_notify_obs_open(&bridge, slot2->observation_id, h2);

    /* Re-associate same candidate to Observation B — no second ENTER. */
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 80u);
    EXPECT(strcmp(ce_bridge_bound_observation_id(&bridge), slot2->observation_id) == 0);
    EXPECT(bridge.assoc.ep_handle.generation == h2.generation);
    EXPECT(bridge.assoc.ep_handle.index == h2.index);

    ce_bridge_diag_t d_re;
    ce_bridge_get_diag(&bridge, &d_re);
    EXPECT(d_re.promote_calls == 1u);
    EXPECT(d_re.enter_queued == 1u);
    EXPECT(d_re.assoc_success == 2u);
    EXPECT(d_re.exit_queued == 0u);

    /*
     * Inject a stale episode handle into the live association. Poll must drop
     * it, then Model-A rebind to the current valid Observation B handle —
     * never keep generation from episode 1.
     */
    EXPECT(!sp_episode_registry_handle_valid(&reg, h1));
    bridge.assoc.ep_handle = h1;
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(bridge.assoc.ep_handle.generation == h2.generation);
    EXPECT(bridge.assoc.ep_handle.index == h2.index);
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 80u);
    EXPECT(strcmp(ce_bridge_bound_observation_id(&bridge), slot2->observation_id)
           == 0);
}

static void test_enter_idempotent(void)
{
    ce_engine_t ce;
    ce_promotion_bridge_t bridge;
    sp_episode_registry_t reg;
    sp_ep_handle_t h;

    ce_engine_ready(&ce);
    plant_candidate(&ce, 90u, AE_CAND_ACTIVE, 3u, -40.0f, AE_QUALITY_OK);
    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);
    sp_episode_registry_init(&reg);
    open_obs(&bridge, &reg, &h, fake_uuid_a);

    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_is_bound(&bridge));
    ce_bridge_diag_t d0;
    ce_bridge_get_diag(&bridge, &d0);
    EXPECT(d0.assoc_success == 1u);

    ae_candidate_t c;
    memset(&c, 0, sizeof(c));
    c.candidate_id = 90u;
    ce_bridge_on_promotion(&c, CE_PROMOTION_ENTER, &bridge);
    ce_bridge_on_promotion(&c, CE_PROMOTION_ENTER, &bridge);
    ce_bridge_poll(&bridge, &reg);

    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 90u);
    ce_bridge_diag_t d1;
    ce_bridge_get_diag(&bridge, &d1);
    EXPECT(d1.assoc_success == 1u);
    EXPECT(d1.enter_idempotent >= 2u);
}

static void test_exit_idempotent_and_stale(void)
{
    ce_engine_t ce;
    ce_promotion_bridge_t bridge;
    sp_episode_registry_t reg;
    sp_ep_handle_t h;

    ce_engine_ready(&ce);
    plant_candidate(&ce, 100u, AE_CAND_ACTIVE, 5u, -25.0f, AE_QUALITY_OK);
    plant_candidate(&ce, 101u, AE_CAND_ACTIVE, 3u, -25.0f, AE_QUALITY_OK);
    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);
    sp_episode_registry_init(&reg);
    open_obs(&bridge, &reg, &h, fake_uuid_a);

    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 100u);

    ae_candidate_t ex;
    memset(&ex, 0, sizeof(ex));
    ex.candidate_id = 100u;
    ce_bridge_on_promotion(&ex, CE_PROMOTION_EXIT, &bridge);
    /* Mirror expire_slot: EXIT ends the CE promotion lifecycle for A. */
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (ce.slots[i].in_use && ce.slots[i].candidate_id == 100u) {
            ce.slots[i].promotion_entered = false;
            ce.slots[i].in_use = false;
            ce.slots[i].state = AE_CAND_EXPIRED;
        }
    }
    /* Obs still open: next ACTIVE (101) may ENTER now that A is gone. */
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 101u);

    ce_bridge_diag_t d_b;
    ce_bridge_get_diag(&bridge, &d_b);
    EXPECT(d_b.enter_queued == 2u);
    EXPECT(d_b.promote_calls == 2u);

    /* Second / stale EXIT for A: must not clear B. */
    ce_bridge_on_promotion(&ex, CE_PROMOTION_EXIT, &bridge);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 101u);

    ce_bridge_on_promotion(&ex, CE_PROMOTION_EXIT, &bridge);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 101u);
}

/* -------------------------------------------------------------------------- */
/* Observation authority                                                      */
/* -------------------------------------------------------------------------- */

static void test_obs_authority_bridge_does_not_allocate_or_end(void)
{
    ce_engine_t ce;
    ce_promotion_bridge_t bridge;
    sp_episode_registry_t reg;

    ce_engine_ready(&ce);
    plant_candidate(&ce, 110u, AE_CAND_ACTIVE, 4u, -40.0f, AE_QUALITY_OK);
    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);
    sp_episode_registry_init(&reg);

    EXPECT(sp_episode_registry_in_use_count(&reg) == 0u);
    ce_bridge_poll(&bridge, &reg);
    /* No obs context → no promote/assoc and registry untouched. */
    EXPECT(sp_episode_registry_in_use_count(&reg) == 0u);
    EXPECT(!ce_bridge_is_bound(&bridge));

    sp_ep_handle_t h;
    open_obs(&bridge, &reg, &h, fake_uuid_a);
    EXPECT(sp_episode_registry_in_use_count(&reg) == 1u);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_is_bound(&bridge));
    /* Bridge must not allocate a second episode. */
    EXPECT(sp_episode_registry_in_use_count(&reg) == 1u);

    char oid_before[SP_OBS_EPISODE_ID_LEN];
    memset(oid_before, 0, sizeof(oid_before));
    {
        const char *oid = ce_bridge_bound_observation_id(&bridge);
        size_t n = strlen(oid);
        if (n >= sizeof(oid_before)) {
            n = sizeof(oid_before) - 1u;
        }
        memcpy(oid_before, oid, n);
    }

    /* Poll again: still one episode; bridge does not END/release. */
    ce_bridge_poll(&bridge, &reg);
    EXPECT(sp_episode_registry_in_use_count(&reg) == 1u);
    EXPECT(sp_episode_registry_handle_valid(&reg, h));
    EXPECT(strcmp(ce_bridge_bound_observation_id(&bridge), oid_before) == 0);

    /* Only explicit RMS-style close releases. */
    ce_bridge_notify_obs_closed(&bridge);
    sp_episode_registry_release(&reg, h);
    EXPECT(sp_episode_registry_in_use_count(&reg) == 0u);
    EXPECT(!ce_bridge_is_bound(&bridge));
}

/* -------------------------------------------------------------------------- */
/* Queue — artificial host saturation vs device model                         */
/* -------------------------------------------------------------------------- */

static void test_queue_overflow_artificial_host_saturation(void)
{
    /*
     * Artificial: host repeatedly calls the callback to fill the ring.
     * This is NOT the device model (single-task serialized activity_task;
     * see ce_promotion_bridge.h). It only proves defensive overflow counting.
     */
    ce_promotion_bridge_t bridge;
    ce_bridge_init(&bridge);
    ae_candidate_t c;
    memset(&c, 0, sizeof(c));
    for (uint32_t i = 0; i < CE_BRIDGE_QUEUE_CAP + 3u; i++) {
        c.candidate_id = i + 1u;
        ce_bridge_on_promotion(&c, CE_PROMOTION_ENTER, &bridge);
    }
    ce_bridge_diag_t d;
    ce_bridge_get_diag(&bridge, &d);
    EXPECT(d.queue_overflows == 3u);
    EXPECT(d.enter_queued == CE_BRIDGE_QUEUE_CAP);
}

/* -------------------------------------------------------------------------- */
/* Integration smoke + CE diag (retained)                                     */
/* -------------------------------------------------------------------------- */

/**
 * Real AE/CE path: promote → GAP(COASTING) → rematch → stays associated;
 * no second ENTER; second candidate cannot overwrite; expiry → one EXIT.
 */
static void test_promoted_coast_rematch_keeps_assoc(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ce_promotion_bridge_t bridge;
    sp_episode_registry_t reg;
    sp_ep_handle_t h;
    ae_config_t acfg;
    ce_config_t ccfg;

    ae_config_set_defaults(&acfg);
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 2u;
    ccfg.t_coast = 3u;
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_start(&ce, &ccfg));
    EXPECT(ae_add_signature_listener(&ae, ce_on_signature, &ce));

    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);
    sp_episode_registry_init(&reg);

    publish_tone(&ae, 1u, -40.0f, false);
    publish_tone(&ae, 2u, -40.0f, false);
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    const uint64_t id = c.candidate_id;

    open_obs(&bridge, &reg, &h, fake_uuid_a);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == id);
    const char *obs_id = ce_bridge_bound_observation_id(&bridge);
    EXPECT(obs_id != NULL && obs_id[0] != '\0');
    char obs_copy[SP_OBS_EPISODE_ID_LEN];
    memcpy(obs_copy, obs_id, sizeof(obs_copy));

    ce_bridge_diag_t d0;
    ce_bridge_get_diag(&bridge, &d0);
    EXPECT(d0.assoc_success == 1u);
    EXPECT(d0.enter_queued == 1u);

    publish_tone(&ae, 3u, -40.0f, true);
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_COASTING);
    EXPECT(c.promotion_entered);
    uint64_t primary = 0u;
    EXPECT(!ce_bridge_select_primary(&ce, &primary));
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == id);

    /* Rematch restores PROMOTED; no second ENTER. */
    publish_tone(&ae, 4u, -40.0f, false);
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_PROMOTED);
    EXPECT(c.promotion_entered);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == id);
    EXPECT(strcmp(ce_bridge_bound_observation_id(&bridge), obs_copy) == 0);
    ce_bridge_diag_t d1;
    ce_bridge_get_diag(&bridge, &d1);
    EXPECT(d1.enter_queued == 1u);
    EXPECT(d1.assoc_success == 1u);

    /* Hostile ENTER for another id must not overwrite live association. */
    ae_candidate_t hostile;
    memset(&hostile, 0, sizeof(hostile));
    hostile.candidate_id = id + 99u;
    ce_bridge_on_promotion(&hostile, CE_PROMOTION_ENTER, &bridge);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == id);
    ce_bridge_diag_t d2;
    ce_bridge_get_diag(&bridge, &d2);
    EXPECT(d2.assoc_reject_overwrite >= 1u);

    /* Expiry after rematch: exactly one EXIT clears association. */
    publish_tone(&ae, 5u, -40.0f, true);
    publish_tone(&ae, 6u, -40.0f, true);
    publish_tone(&ae, 7u, -40.0f, true);
    publish_tone(&ae, 8u, -40.0f, true);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_candidate_count(&ce) == 0u);
    EXPECT(!ce_bridge_is_bound(&bridge));
    ce_bridge_diag_t d3;
    ce_bridge_get_diag(&bridge, &d3);
    EXPECT(d3.exit_queued == 1u);
    EXPECT(d3.disassoc_on_exit == 1u);

    ce_bridge_diag_snapshot_t snap;
    ce_bridge_get_diag_snapshot(&bridge, &snap);
    EXPECT(!snap.bound);
    EXPECT(snap.candidate_id == 0u);
    EXPECT(snap.observation_id[0] == '\0');
}

static void test_assoc_with_obs_promotes_primary(void)
{
    ce_engine_t ce;
    ce_promotion_bridge_t bridge;
    sp_episode_registry_t reg;
    sp_ep_handle_t h;

    ce_engine_ready(&ce);
    plant_candidate(&ce, 1u, AE_CAND_ACTIVE, 2u, -35.0f, AE_QUALITY_OK);
    plant_candidate(&ce, 2u, AE_CAND_ACTIVE, 2u, -60.0f, AE_QUALITY_OK);
    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);
    sp_episode_registry_init(&reg);
    open_obs(&bridge, &reg, &h, fake_uuid_a);

    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 1u);
    ae_candidate_t c;
    EXPECT(ce_find_candidate(&ce, 1u, &c));
    EXPECT(c.state == AE_CAND_PROMOTED);
}

static void test_obs_end_clears_assoc(void)
{
    ce_engine_t ce;
    ce_promotion_bridge_t bridge;
    sp_episode_registry_t reg;
    sp_ep_handle_t h;

    ce_engine_ready(&ce);
    plant_candidate(&ce, 3u, AE_CAND_ACTIVE, 2u, -40.0f, AE_QUALITY_OK);
    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);
    sp_episode_registry_init(&reg);
    open_obs(&bridge, &reg, &h, fake_uuid_a);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_is_bound(&bridge));

    ce_bridge_notify_obs_closed(&bridge);
    EXPECT(!ce_bridge_is_bound(&bridge));
    ce_bridge_diag_t d;
    ce_bridge_get_diag(&bridge, &d);
    EXPECT(d.disassoc_on_obs_end == 1u);
    EXPECT(d.exit_queued == 0u);

    /* CE promotion lifecycle is not cleared by Observation close. */
    ae_candidate_t c;
    EXPECT(ce_find_candidate(&ce, 3u, &c));
    EXPECT(c.state == AE_CAND_PROMOTED);
    EXPECT(c.promotion_entered);
}

/* -------------------------------------------------------------------------- */
/* Model A — re-associate promoted candidate across Observations              */
/* -------------------------------------------------------------------------- */

static void test_reassoc_promoted_across_observations(void)
{
    ce_engine_t ce;
    ce_promotion_bridge_t bridge;
    sp_episode_registry_t reg;
    sp_ep_handle_t h_a;
    sp_ep_handle_t h_b;

    ce_engine_ready(&ce);
    /* 300 wins initial ranking; 301 stays a live ACTIVE that must not steal primary. */
    plant_candidate(&ce, 300u, AE_CAND_ACTIVE, 8u, -20.0f, AE_QUALITY_OK);
    plant_candidate(&ce, 301u, AE_CAND_ACTIVE, 4u, -10.0f, AE_QUALITY_OK);
    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);
    sp_episode_registry_init(&reg);

    open_obs(&bridge, &reg, &h_a, fake_uuid_a);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 300u);

    ce_bridge_diag_t d0;
    ce_bridge_get_diag(&bridge, &d0);
    EXPECT(d0.promote_calls == 1u);
    EXPECT(d0.enter_queued == 1u);

    ce_bridge_notify_obs_closed(&bridge);
    sp_episode_registry_release(&reg, h_a);
    EXPECT(!ce_bridge_is_bound(&bridge));

    ae_candidate_t x;
    EXPECT(ce_find_candidate(&ce, 300u, &x));
    EXPECT(x.promotion_entered);
    EXPECT(x.state == AE_CAND_PROMOTED);

    /* Stronger ACTIVE must not become primary while X remains promoted. */
    uint64_t primary = 0u;
    EXPECT(!ce_bridge_select_primary(&ce, &primary));

    open_obs(&bridge, &reg, &h_b, fake_uuid_b);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 300u);
    EXPECT(strcmp(ce_bridge_bound_observation_id(&bridge),
                  "22222222-2222-2222-2222-222222222222")
           == 0);
    EXPECT(bridge.assoc.ep_handle.generation == h_b.generation);

    ce_bridge_diag_t d1;
    ce_bridge_get_diag(&bridge, &d1);
    EXPECT(d1.promote_calls == 1u);
    EXPECT(d1.enter_queued == 1u);
    EXPECT(d1.assoc_success == 2u);
    EXPECT(d1.exit_queued == 0u);

    ae_candidate_t y;
    EXPECT(ce_find_candidate(&ce, 301u, &y));
    EXPECT(y.state == AE_CAND_ACTIVE);
}

static void test_reassoc_coasting_with_promotion_entered(void)
{
    ce_engine_t ce;
    ce_promotion_bridge_t bridge;
    sp_episode_registry_t reg;
    sp_ep_handle_t h_a;
    sp_ep_handle_t h_b;

    ce_engine_ready(&ce);
    ae_candidate_t *slot =
        plant_candidate(&ce, 310u, AE_CAND_ACTIVE, 3u, -40.0f, AE_QUALITY_OK);
    EXPECT(slot != NULL);
    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);
    sp_episode_registry_init(&reg);

    open_obs(&bridge, &reg, &h_a, fake_uuid_a);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 310u);

    ce_bridge_notify_obs_closed(&bridge);
    sp_episode_registry_release(&reg, h_a);

    /* Temporary COASTING with sticky promotion_entered. */
    slot->state = AE_CAND_COASTING;
    EXPECT(slot->promotion_entered);

    open_obs(&bridge, &reg, &h_b, fake_uuid_b);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 310u);
    EXPECT(strcmp(ce_bridge_bound_observation_id(&bridge),
                  "22222222-2222-2222-2222-222222222222")
           == 0);

    ce_bridge_diag_t d;
    ce_bridge_get_diag(&bridge, &d);
    EXPECT(d.promote_calls == 1u);
    EXPECT(d.enter_queued == 1u);
    EXPECT(d.assoc_success == 2u);
}

static void test_reassoc_after_obs_open_with_no_candidates_then_enter(void)
{
    ce_engine_t ce;
    ce_promotion_bridge_t bridge;
    sp_episode_registry_t reg;
    sp_ep_handle_t h;

    ce_engine_ready(&ce);
    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);
    sp_episode_registry_init(&reg);

    open_obs(&bridge, &reg, &h, fake_uuid_a);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(!ce_bridge_is_bound(&bridge));

    plant_candidate(&ce, 320u, AE_CAND_ACTIVE, 2u, -40.0f, AE_QUALITY_OK);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 320u);

    ce_bridge_diag_t d;
    ce_bridge_get_diag(&bridge, &d);
    EXPECT(d.promote_calls == 1u);
    EXPECT(d.enter_queued == 1u);
    EXPECT(d.assoc_success == 1u);
}

static void test_reassoc_does_not_bind_expired_or_stale_identity(void)
{
    ce_engine_t ce;
    ce_promotion_bridge_t bridge;
    sp_episode_registry_t reg;
    sp_ep_handle_t h;

    ce_config_t ccfg;
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 2u;
    ccfg.t_coast = 1u;
    EXPECT(ce_start(&ce, &ccfg));

    plant_candidate(&ce, 330u, AE_CAND_ACTIVE, 3u, -40.0f, AE_QUALITY_OK);
    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);
    sp_episode_registry_init(&reg);

    open_obs(&bridge, &reg, &h, fake_uuid_a);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 330u);

    ce_bridge_notify_obs_closed(&bridge);
    sp_episode_registry_release(&reg, h);

    feed_ce_gap(&ce, 1u);
    feed_ce_gap(&ce, 2u);
    EXPECT(ce_candidate_count(&ce) == 0u);

    open_obs(&bridge, &reg, &h, fake_uuid_b);
    ce_bridge_poll(&bridge, &reg);
    /* No live promoted lifecycle → unbound until a new ACTIVE is promoted. */
    EXPECT(!ce_bridge_is_bound(&bridge));

    plant_candidate(&ce, 331u, AE_CAND_ACTIVE, 3u, -40.0f, AE_QUALITY_OK);
    ce_bridge_poll(&bridge, &reg);
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == 331u);
    EXPECT(ce_bridge_bound_candidate_id(&bridge) != 330u);

    ce_bridge_diag_t d;
    ce_bridge_get_diag(&bridge, &d);
    EXPECT(d.exit_queued == 1u);
    EXPECT(d.promote_calls == 2u);
    EXPECT(d.enter_queued == 2u);
}

static void test_ce_diag_counters(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ae_config_t acfg;
    ce_config_t ccfg;
    ae_config_set_defaults(&acfg);
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 2u;
    ccfg.t_coast = 1u;
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_start(&ce, &ccfg));
    EXPECT(ae_add_signature_listener(&ae, ce_on_signature, &ce));
    publish_tone(&ae, 1u, -40.0f, false);
    publish_tone(&ae, 2u, -40.0f, false);
    ce_diag_t d;
    ce_get_diag(&ce, &d);
    EXPECT(d.created == 1u);
    EXPECT(d.state_hist[AE_CAND_ACTIVE] == 1u);

    publish_tone(&ae, 3u, -40.0f, true);
    publish_tone(&ae, 4u, -40.0f, true);
    ce_get_diag(&ce, &d);
    EXPECT(d.expired >= 1u);
    EXPECT(d.in_use_count == 0u);
}

int main(void)
{
    test_rank_by_evidence_count();
    test_rank_by_ema_rms_tiebreak();
    test_rank_by_lower_candidate_id_tiebreak();

    test_eligibility_nascent_not_selected();
    test_eligibility_coasting_not_selected();
    test_eligibility_promoted_blocks_new_primary();
    test_eligibility_promotion_entered_coasting_blocks();
    test_eligibility_quality_ok_and_degraded_accepted();
    test_eligibility_unsupported_quality_rejected();
    test_eligibility_no_promote_without_obs_context();
    test_eligibility_no_second_promote_while_promoted();

    test_candidate_slot_reuse_no_identity_confusion();
    test_episode_generation_reuse_cannot_affect_new_assoc();
    test_enter_idempotent();
    test_exit_idempotent_and_stale();

    test_obs_authority_bridge_does_not_allocate_or_end();
    test_queue_overflow_artificial_host_saturation();

    test_promoted_coast_rematch_keeps_assoc();
    test_assoc_with_obs_promotes_primary();
    test_obs_end_clears_assoc();
    test_reassoc_promoted_across_observations();
    test_reassoc_coasting_with_promotion_entered();
    test_reassoc_after_obs_open_with_no_candidates_then_enter();
    test_reassoc_does_not_bind_expired_or_stale_identity();
    test_ce_diag_counters();

    if (g_failures != 0) {
        fprintf(stderr, "test_ce_promotion_bridge: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ce_promotion_bridge: ok\n");
    return 0;
}
