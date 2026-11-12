/**
 * Host tests: Candidate Engine lifecycle over AE signatures.
 *
 * From repo root:
 *   cc -std=c11 -Wall -Wextra -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/candidate_engine/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     firmware/products/dbmap/ear/ear_acoustic/acoustic_l0.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_history.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_time.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_engine.c \
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

static void test_two_sources(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ae_config_t acfg;
    ce_config_t ccfg;
    ae_config_set_defaults(&acfg);
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 2u;
    ccfg.match_distance_max = 6.0f;
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_start(&ce, &ccfg));
    EXPECT(ae_add_signature_listener(&ae, ce_on_signature, &ce));

    publish_tone(&ae, 1u, -35.0f, false);
    publish_tone(&ae, 2u, -60.0f, false); /* far → second nascent */
    EXPECT(ce_candidate_count(&ce) == 2u);

    publish_tone(&ae, 3u, -35.2f, false);
    publish_tone(&ae, 4u, -59.5f, false);
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

int main(void)
{
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
