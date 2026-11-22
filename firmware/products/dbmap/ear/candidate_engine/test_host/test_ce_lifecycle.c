/**
 * Host tests: Candidate Engine lifecycle over AE signatures.
 *
 * From repo root:
 *   cc -std=c11 -Wall -Wextra -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/acoustic_profile/include \
 *     -I firmware/products/dbmap/ear/candidate_engine/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     -I firmware/platform/audio/include \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_history.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_time.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_engine.c \
 *     firmware/products/dbmap/ear/acoustic_profile/as_profile_as001.c \
 *     firmware/products/dbmap/ear/acoustic_profile/as_profile_registry.c \
 *     firmware/products/dbmap/ear/acoustic_profile/as_profile_match.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_decision.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_module.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_module_profile.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_workflow.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_manager.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_engine.c \
 *     firmware/products/dbmap/ear/candidate_engine/test_host/test_ce_lifecycle.c \
 *     -lm -o /tmp/test_ce_m1 && /tmp/test_ce_m1
 */
#include "ae_engine.h"
#include "ae_signature.h"
#include "ce_engine.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_promo_enter = 0;
static int g_promo_exit = 0;
static uint64_t g_last_exit_id = 0;
static uint64_t g_last_enter_id = 0;

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

static void publish_tone(
    ae_engine_t *ae,
    uint32_t seq,
    float rms,
    bool gap
)
{
    dbmap_l0_window_t l0;
    fill_l0(&l0, rms, (int64_t)seq * 1000000);
    ae_signature_t sig;
    EXPECT(ae_signature_build(seq, &l0, NULL, NULL, NULL, gap, &sig));
    EXPECT(ae_publish_signature(ae, &sig));
}

static void on_promo(const ae_candidate_t *c, ce_promotion_event_t ev, void *ctx)
{
    (void)ctx;
    if (ev == CE_PROMOTION_ENTER) {
        g_promo_enter++;
        g_last_enter_id = c->candidate_id;
    } else if (ev == CE_PROMOTION_EXIT) {
        g_promo_exit++;
        g_last_exit_id = c->candidate_id;
    }
}

static void reset_promo_counts(void)
{
    g_promo_enter = 0;
    g_promo_exit = 0;
    g_last_exit_id = 0;
    g_last_enter_id = 0;
}

static void setup_ae_ce(
    ae_engine_t *ae,
    ce_engine_t *ce,
    ce_config_t *ccfg_out,
    uint32_t t_coast
)
{
    ae_config_t acfg;
    ce_config_t ccfg;
    ae_config_set_defaults(&acfg);
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 2u;
    ccfg.t_coast = t_coast;
    EXPECT(ae_start(ae, &acfg));
    EXPECT(ce_start(ce, &ccfg));
    EXPECT(ae_add_signature_listener(ae, ce_on_signature, ce));
    if (ccfg_out != NULL) {
        *ccfg_out = ccfg;
    }
}

static void activate_one(ae_engine_t *ae, ce_engine_t *ce, ae_candidate_t *out)
{
    publish_tone(ae, 1u, -40.0f, false);
    publish_tone(ae, 2u, -40.0f, false);
    EXPECT(ce_get_candidate(ce, 0, out));
    EXPECT(out->state == AE_CAND_ACTIVE);
}

static void test_nascent_to_active(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL, 3u);

    publish_tone(&ae, 1u, -40.0f, false);
    EXPECT(ce_candidate_count(&ce) == 1u);
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_NASCENT);

    publish_tone(&ae, 2u, -39.5f, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    EXPECT(c.evidence_count >= 2u);
}

static void test_coast_and_expire(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL, 2u);

    publish_tone(&ae, 1u, -40.0f, false);
    publish_tone(&ae, 2u, -40.0f, false);
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);

    publish_tone(&ae, 3u, -40.0f, true); /* GAP */
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_COASTING);

    publish_tone(&ae, 4u, -40.0f, true);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_COASTING);

    publish_tone(&ae, 5u, -40.0f, true); /* coast exceeded → expired / free */
    EXPECT(ce_candidate_count(&ce) == 0u);
}

/**
 * Two concurrent candidates via distinct relative band shapes (Stage 1:
 * level alone must not be the identity key).
 */
static void test_two_sources(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ae_config_t acfg;
    ce_config_t ccfg;
    ae_config_set_defaults(&acfg);
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 2u;
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_start(&ce, &ccfg));
    EXPECT(ae_add_signature_listener(&ae, ce_on_signature, &ce));

    dbmap_l0_window_t l0;
    ae_signature_t sig;

    fill_l0(&l0, -40.0f, 1000000);
    EXPECT(ae_signature_build(1u, &l0, NULL, NULL, NULL, false, &sig));
    EXPECT(ae_publish_signature(&ae, &sig));

    /*
     * Distinct relative band shape at same RMS → second candidate.
     * Shape Δ large enough that identity distance > match_max + hysteresis.
     */
    fill_l0(&l0, -40.0f, 2000000);
    l0.band_dbfs[0] = -40.0f - 10.0f;
    l0.band_dbfs[1] = -40.0f - 10.0f;
    l0.band_dbfs[2] = -40.0f + 5.0f;
    l0.band_dbfs[3] = -40.0f + 8.0f;
    EXPECT(ae_signature_build(2u, &l0, NULL, NULL, NULL, false, &sig));
    EXPECT(ae_publish_signature(&ae, &sig));
    EXPECT(ce_candidate_count(&ce) == 2u);

    fill_l0(&l0, -40.0f, 3000000);
    EXPECT(ae_signature_build(3u, &l0, NULL, NULL, NULL, false, &sig));
    EXPECT(ae_publish_signature(&ae, &sig));

    fill_l0(&l0, -40.0f, 4000000);
    l0.band_dbfs[0] = -40.0f - 10.0f;
    l0.band_dbfs[1] = -40.0f - 10.0f;
    l0.band_dbfs[2] = -40.0f + 5.0f;
    l0.band_dbfs[3] = -40.0f + 8.0f;
    EXPECT(ae_signature_build(4u, &l0, NULL, NULL, NULL, false, &sig));
    EXPECT(ae_publish_signature(&ae, &sig));
    EXPECT(ce_candidate_count(&ce) == 2u);

    uint8_t active = 0u;
    for (uint8_t i = 0; i < ce_candidate_count(&ce); i++) {
        ae_candidate_t c;
        EXPECT(ce_get_candidate(&ce, i, &c));
        if (c.state == AE_CAND_ACTIVE) {
            active++;
        }
    }
    EXPECT(active == 2u);
}

static void test_promote(void)
{
    reset_promo_counts();
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL, 3u);
    ce_set_promotion_cb(&ce, on_promo, NULL);

    ae_candidate_t c;
    activate_one(&ae, &ce, &c);
    EXPECT(ce_promote(&ce, c.candidate_id));
    EXPECT(ce_find_candidate(&ce, c.candidate_id, &c));
    EXPECT(c.state == AE_CAND_PROMOTED);
    EXPECT(c.promotion_entered);
    EXPECT(g_promo_enter == 1);
    EXPECT(g_last_enter_id == c.candidate_id);
    EXPECT(g_promo_exit == 0);
}

/**
 * Production expiry always coasts first (PROMOTED → COASTING → EXPIRED).
 * Prove EXIT still fires exactly once for a promoted candidate.
 */
static void test_promoted_expire_emits_one_exit(void)
{
    reset_promo_counts();
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL, 1u);
    ce_set_promotion_cb(&ce, on_promo, NULL);

    ae_candidate_t c;
    activate_one(&ae, &ce, &c);
    const uint64_t id = c.candidate_id;
    EXPECT(ce_promote(&ce, id));
    EXPECT(g_promo_enter == 1);

    /* GAP: PROMOTED → COASTING; further GAP exceeds t_coast → EXPIRED + EXIT */
    publish_tone(&ae, 3u, -40.0f, true);
    publish_tone(&ae, 4u, -40.0f, true);

    EXPECT(ce_candidate_count(&ce) == 0u);
    EXPECT(g_promo_exit == 1);
    EXPECT(g_last_exit_id == id);

    ce_diag_t d;
    ce_get_diag(&ce, &d);
    EXPECT(d.promotion_enter_events == 1u);
    EXPECT(d.promotion_exit_events == 1u);
}

/**
 * Explicit mid-state assertion: EXIT must fire when expiry happens from COASTING
 * after a prior promotion (the gap this fix closes).
 */
static void test_promoted_coast_then_expire_emits_one_exit(void)
{
    reset_promo_counts();
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL, 2u);
    ce_set_promotion_cb(&ce, on_promo, NULL);

    ae_candidate_t c;
    activate_one(&ae, &ce, &c);
    const uint64_t id = c.candidate_id;
    EXPECT(ce_promote(&ce, id));

    publish_tone(&ae, 3u, -40.0f, true);
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_COASTING);
    EXPECT(c.promotion_entered);
    EXPECT(g_promo_exit == 0);

    publish_tone(&ae, 4u, -40.0f, true);
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_COASTING);
    EXPECT(g_promo_exit == 0);

    publish_tone(&ae, 5u, -40.0f, true);
    EXPECT(ce_candidate_count(&ce) == 0u);
    EXPECT(g_promo_exit == 1);
    EXPECT(g_last_exit_id == id);
}

/** Never-promoted ACTIVE → COASTING → EXPIRED must not emit EXIT. */
static void test_unpromoted_coast_expire_no_exit(void)
{
    reset_promo_counts();
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL, 1u);
    ce_set_promotion_cb(&ce, on_promo, NULL);

    ae_candidate_t c;
    activate_one(&ae, &ce, &c);
    EXPECT(!c.promotion_entered);

    publish_tone(&ae, 3u, -40.0f, true);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_COASTING);

    publish_tone(&ae, 4u, -40.0f, true);
    EXPECT(ce_candidate_count(&ce) == 0u);
    EXPECT(g_promo_enter == 0);
    EXPECT(g_promo_exit == 0);

    ce_diag_t d;
    ce_get_diag(&ce, &d);
    EXPECT(d.promotion_exit_events == 0u);
}

/** Extra GAP / cleanup after expiry must not re-fire EXIT. */
static void test_no_duplicate_exit_after_expire(void)
{
    reset_promo_counts();
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL, 1u);
    ce_set_promotion_cb(&ce, on_promo, NULL);

    ae_candidate_t c;
    activate_one(&ae, &ce, &c);
    const uint64_t id = c.candidate_id;
    EXPECT(ce_promote(&ce, id));

    publish_tone(&ae, 3u, -40.0f, true);
    publish_tone(&ae, 4u, -40.0f, true);
    EXPECT(g_promo_exit == 1);

    publish_tone(&ae, 5u, -40.0f, true);
    publish_tone(&ae, 6u, -40.0f, true);
    EXPECT(g_promo_exit == 1);
    EXPECT(!ce_promote(&ce, id));

    ce_diag_t d;
    ce_get_diag(&ce, &d);
    EXPECT(d.promotion_exit_events == 1u);
}

/**
 * PROMOTED → COASTING → rematch must restore PROMOTED (via promotion_entered)
 * without a second ENTER.
 */
static void test_promoted_coast_rematch_restores_promoted(void)
{
    reset_promo_counts();
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL, 3u);
    ce_set_promotion_cb(&ce, on_promo, NULL);

    ae_candidate_t c;
    activate_one(&ae, &ce, &c);
    const uint64_t id = c.candidate_id;
    EXPECT(ce_promote(&ce, id));
    EXPECT(g_promo_enter == 1);

    publish_tone(&ae, 3u, -40.0f, true);
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_COASTING);
    EXPECT(c.promotion_entered);

    publish_tone(&ae, 4u, -40.0f, false);
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_PROMOTED);
    EXPECT(c.promotion_entered);
    EXPECT(g_promo_enter == 1);
    EXPECT(g_promo_exit == 0);
    EXPECT(!ce_promote(&ce, id));

    /* Still exactly one EXIT after later coast expiry. */
    publish_tone(&ae, 5u, -40.0f, true);
    publish_tone(&ae, 6u, -40.0f, true);
    publish_tone(&ae, 7u, -40.0f, true);
    publish_tone(&ae, 8u, -40.0f, true);
    EXPECT(ce_candidate_count(&ce) == 0u);
    EXPECT(g_promo_exit == 1);
    EXPECT(g_last_exit_id == id);
}

/** Unpromoted COASTING → rematch stays ACTIVE and never acquires promotion. */
static void test_unpromoted_coast_rematch_stays_active(void)
{
    reset_promo_counts();
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL, 3u);
    ce_set_promotion_cb(&ce, on_promo, NULL);

    ae_candidate_t c;
    activate_one(&ae, &ce, &c);
    const uint64_t id = c.candidate_id;

    publish_tone(&ae, 3u, -40.0f, true);
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_COASTING);
    EXPECT(!c.promotion_entered);

    publish_tone(&ae, 4u, -40.0f, false);
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    EXPECT(!c.promotion_entered);
    EXPECT(g_promo_enter == 0);
    EXPECT(g_promo_exit == 0);
}

/**
 * Slot reuse after a promoted candidate expires: new identity must not inherit
 * promotion_entered or emit a stale EXIT when it later coasts out unpromoted.
 */
static void test_slot_reuse_clears_promotion_entered(void)
{
    reset_promo_counts();
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL, 1u);
    ce_set_promotion_cb(&ce, on_promo, NULL);

    ae_candidate_t c;
    activate_one(&ae, &ce, &c);
    const uint64_t old_id = c.candidate_id;
    EXPECT(ce_promote(&ce, old_id));

    publish_tone(&ae, 3u, -40.0f, true);
    publish_tone(&ae, 4u, -40.0f, true);
    EXPECT(g_promo_exit == 1);
    EXPECT(g_last_exit_id == old_id);
    EXPECT(ce_candidate_count(&ce) == 0u);

    /* New candidate in (likely) same slot via normal create path. */
    publish_tone(&ae, 5u, -40.0f, false);
    publish_tone(&ae, 6u, -40.0f, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.candidate_id != old_id);
    EXPECT(c.state == AE_CAND_ACTIVE);
    EXPECT(!c.promotion_entered);

    const uint64_t new_id = c.candidate_id;
    publish_tone(&ae, 7u, -40.0f, true);
    publish_tone(&ae, 8u, -40.0f, true);
    EXPECT(ce_candidate_count(&ce) == 0u);
    EXPECT(g_promo_exit == 1); /* no second EXIT for unpromoted reuse */
    EXPECT(g_last_exit_id == old_id);
    EXPECT(!ce_find_candidate(&ce, new_id, &c));

    ce_diag_t d;
    ce_get_diag(&ce, &d);
    EXPECT(d.promotion_exit_events == 1u);
}

/* Only the spectral identity varies: 600 Hz spacing gives distance 18. */
static ae_signature_t spectral_window(uint32_t seq, float hz, uint8_t quality)
{
    dbmap_l0_window_t l0;
    fill_l0(&l0, -35.0f, (int64_t)seq * 1000000);
    ae_signature_t sig = {0};
    EXPECT(ae_signature_build(seq, &l0, NULL, NULL, NULL, false, &sig));
    sig.quality = quality;
    sig.spectral_valid = true;
    sig.dominant_hz = hz;
    sig.centroid_hz = hz;
    sig.flatness = 0.1f;
    return sig;
}

static void publish_spectral(ae_engine_t *ae, uint32_t seq, float hz)
{
    const ae_signature_t sig = spectral_window(seq, hz, AE_QUALITY_OK);
    EXPECT(ae_publish_signature(ae, &sig));
}

static ae_candidate_t lookup(ce_engine_t *ce, uint64_t id)
{
    ae_candidate_t candidate = {0};
    EXPECT(ce_find_candidate(ce, id, &candidate));
    return candidate;
}

static void test_full_table_recovery(bool continue_last)
{
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL, 3u);
    EXPECT(ce.cfg.valid_miss_limit == 16u);
    uint32_t seq = 0;
    for (unsigned i = 0; i < 8; i++) {
        const float hz = 300.0f + 600.0f * i;
        publish_spectral(&ae, ++seq, hz);
        publish_spectral(&ae, ++seq, hz);
        EXPECT(lookup(&ce, i + 1u).state == AE_CAND_ACTIVE);
    }
    EXPECT(ce_candidate_count(&ce) == 8u);
    const float new_hz = continue_last ? 4500.0f : 5100.0f;
    publish_spectral(&ae, ++seq, new_hz);
    EXPECT(ce.diag.expired == 0u);
    EXPECT(ce.diag.create_dropped_full == (continue_last ? 0u : 1u));
    publish_spectral(&ae, ++seq, new_hz);
    EXPECT(ce.diag.expired == 1u);
    if (!continue_last) {
        EXPECT(lookup(&ce, 9u).state == AE_CAND_NASCENT);
    }
    for (unsigned i = 0; i < 14; i++) {
        publish_spectral(&ae, ++seq, new_hz);
    }
    EXPECT(ce_candidate_count(&ce) == 1u);
    EXPECT(ce.diag.expired == (continue_last ? 7u : 8u));
    const ae_candidate_t current = lookup(&ce, continue_last ? 8u : 9u);
    EXPECT(current.state == AE_CAND_ACTIVE);
    EXPECT(current.gap_events == 0u);
    EXPECT(current.evidence_count == (continue_last ? 18u : 15u));
    ce_stop(&ce);
}

static void test_interleave_and_nascent_retention(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL, 3u);
    ce.cfg.valid_miss_limit = 3u;
    publish_spectral(&ae, 1u, 300.0f);
    publish_spectral(&ae, 2u, 900.0f);
    EXPECT(lookup(&ce, 1u).state == AE_CAND_NASCENT);
    EXPECT(lookup(&ce, 1u).evidence_count == 1u);
    for (uint32_t seq = 3u; seq <= 14u; seq++) {
        publish_spectral(&ae, seq, seq % 2u ? 300.0f : 900.0f);
    }
    EXPECT(lookup(&ce, 1u).evidence_count == 7u);
    EXPECT(lookup(&ce, 1u).state == AE_CAND_ACTIVE);
    EXPECT(lookup(&ce, 2u).evidence_count == 7u);
    EXPECT(lookup(&ce, 2u).state == AE_CAND_ACTIVE);
    publish_spectral(&ae, 15u, 900.0f);
    EXPECT(lookup(&ce, 1u).state == AE_CAND_COASTING);
    publish_spectral(&ae, 16u, 300.0f);
    EXPECT(lookup(&ce, 1u).state == AE_CAND_ACTIVE);
    EXPECT(lookup(&ce, 1u).valid_miss_windows == 0u);
    for (uint32_t seq = 17u; seq <= 19u; seq++) {
        publish_spectral(&ae, seq, 300.0f);
    }
    EXPECT(ce_candidate_count(&ce) == 1u);
    publish_spectral(&ae, 20u, 1500.0f);
    EXPECT(lookup(&ce, 3u).state == AE_CAND_NASCENT);
    for (uint32_t seq = 21u; seq <= 23u; seq++) {
        publish_spectral(&ae, seq, 300.0f);
    }
    EXPECT(ce.diag.expired == 2u);
    EXPECT(ce_candidate_count(&ce) == 1u);
    ce_stop(&ce);
}

static void test_bounded_soft_support(bool ambiguous)
{
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL, 3u);
    ce.cfg.valid_miss_limit = 7u;
    publish_spectral(&ae, 1u, 300.0f);
    publish_spectral(&ae, 2u, 300.0f);
    publish_spectral(&ae, 3u, 1000.0f);
    publish_spectral(&ae, 4u, 1000.0f);
    const float other = ambiguous ? 1500.0f : 1600.0f;
    publish_spectral(&ae, 5u, other);
    publish_spectral(&ae, 6u, other);
    /* Refresh both immediately before presenting the midpoint. */
    publish_spectral(&ae, 7u, 1000.0f);
    publish_spectral(&ae, 8u, other);
    /* Soft support keeps the runner through a second missed hard match. */
    publish_spectral(&ae, 9u, (1000.0f + other) / 2.0f);
    EXPECT(ce.diag.match_ambiguous == (ambiguous ? 1u : 0u));
    EXPECT(ce.diag.match_hysteresis_hold == (ambiguous ? 0u : 1u));
    const ae_candidate_t runner = lookup(&ce, 2u);
    EXPECT(runner.state == AE_CAND_ACTIVE);
    EXPECT(runner.evidence_count == 3u);
    EXPECT(runner.last_window_seq == 7u);
    EXPECT(runner.ema_dominant_hz == 1000.0f);
    EXPECT(lookup(&ce, 3u).evidence_count == (ambiguous ? 4u : 3u));
    EXPECT(lookup(&ce, 3u).state == AE_CAND_ACTIVE);
    ae_candidate_t expired;
    EXPECT(!ce_find_candidate(&ce, 1u, &expired));
    if (!ambiguous) {
        for (uint32_t seq = 10u; seq <= 15u; seq++) {
            publish_spectral(&ae, seq, 1300.0f);
        }
        EXPECT(ce.diag.expired == 3u);
        EXPECT(ce_candidate_count(&ce) == 1u);
        EXPECT(lookup(&ce, 4u).state == AE_CAND_NASCENT);
    }
    ce_stop(&ce);
}

static void test_window_accounting(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL, 3u);
    ce.cfg.valid_miss_limit = 3u;
    publish_spectral(&ae, 1u, 300.0f);
    publish_spectral(&ae, 2u, 300.0f);
    publish_spectral(&ae, 3u, 900.0f);
    const ae_signature_t duplicate = spectral_window(3u, 300.0f, AE_QUALITY_OK);
    const ae_signature_t rewind = spectral_window(2u, 300.0f, AE_QUALITY_OK);
    ae_signature_t zero = duplicate;
    zero.window_seq = 0u;
    const ae_signature_t invalid = spectral_window(4u, 300.0f, AE_QUALITY_INVALID);
    ce_engine_on_signature(&ce, &duplicate);
    ce_engine_on_signature(&ce, &rewind);
    ce_engine_on_signature(&ce, &invalid);
    EXPECT(ce.last_processed_seq == 3u);
    EXPECT(lookup(&ce, 1u).valid_miss_windows == 1u);
    EXPECT(lookup(&ce, 1u).evidence_count == 2u);
    EXPECT(lookup(&ce, 1u).gap_events == 0u);

    const ae_signature_t gap = spectral_window(5u, 300.0f, AE_QUALITY_GAP);
    EXPECT(ae_publish_signature(&ae, &gap));
    ce_engine_on_signature(&ce, &gap);
    EXPECT(ce.diag.seq_discontinuities == 1u);
    EXPECT(ce.diag.quality_gaps == 1u);
    EXPECT(lookup(&ce, 1u).coast_windows == 1u);
    EXPECT(lookup(&ce, 1u).gap_events == 1u);
    EXPECT(lookup(&ce, 1u).valid_miss_windows == 1u);
    publish_spectral(&ae, 11u, 300.0f);
    EXPECT(lookup(&ce, 1u).gap_events == 2u);
    EXPECT(lookup(&ce, 1u).valid_miss_windows == 0u);
    EXPECT(lookup(&ce, 1u).state == AE_CAND_ACTIVE);
    EXPECT(lookup(&ce, 1u).evidence_count == 3u);
    ce_engine_on_signature(&ce, &zero);
    EXPECT(ce.last_processed_seq == 11u);
    EXPECT(lookup(&ce, 1u).evidence_count == 3u);
    ce_stop(&ce);
}

int main(void)
{
    test_full_table_recovery(false);
    test_full_table_recovery(true);
    test_interleave_and_nascent_retention();
    test_bounded_soft_support(true);
    test_bounded_soft_support(false);
    test_window_accounting();
    test_nascent_to_active();
    test_coast_and_expire();
    test_two_sources();
    test_promote();
    test_promoted_expire_emits_one_exit();
    test_promoted_coast_then_expire_emits_one_exit();
    test_unpromoted_coast_expire_no_exit();
    test_no_duplicate_exit_after_expire();
    test_promoted_coast_rematch_restores_promoted();
    test_unpromoted_coast_rematch_stays_active();
    test_slot_reuse_clears_promotion_entered();
    if (g_failures != 0) {
        fprintf(stderr, "test_ce_lifecycle: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ce_lifecycle: ok\n");
    return 0;
}
