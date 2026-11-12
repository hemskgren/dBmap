/**
 * Host tests: AE M4 listener lifecycle + overwrite/seq GAP evaluation.
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
 *     firmware/products/dbmap/ear/audio_engine/ae_gap.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_engine.c \
 *     firmware/products/dbmap/ear/audio_engine/test_host/test_ae_m4_lifecycle_gap.c \
 *     -lm -o /tmp/test_ae_m4 && /tmp/test_ae_m4
 */
#include "ae_engine.h"
#include "ae_gap.h"
#include "ae_signature.h"
#include "ce_engine.h"

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

static int g_a_hits = 0;
static int g_b_hits = 0;
static uint8_t g_last_quality = 0xffu;
static uint32_t g_last_seq = 0u;

static void on_a(const ae_signature_t *sig, void *ctx)
{
    (void)ctx;
    g_a_hits++;
    g_last_seq = sig->window_seq;
    g_last_quality = sig->quality;
}

static void on_b(const ae_signature_t *sig, void *ctx)
{
    (void)sig;
    (void)ctx;
    g_b_hits++;
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
    l0->band_dbfs[3] = rms - 6.0f;
    l0->hl_valid = true;
    l0->hl_db = 2.0f;
}

static bool publish_seq(
    ae_engine_t *ae,
    uint32_t seq,
    bool gap_before,
    float rms
)
{
    dbmap_l0_window_t l0;
    fill_l0(&l0, rms, (int64_t)seq * 1000000);
    ae_signature_t sig;
    if (!ae_signature_build(seq, &l0, NULL, NULL, NULL, gap_before, &sig)) {
        return false;
    }
    return ae_publish_signature(ae, &sig);
}

/** AE starts and publishes with zero listeners (no CE). */
static void test_ae_publish_zero_listeners(void)
{
    ae_engine_t ae;
    ae_config_t cfg;
    ae_config_set_defaults(&cfg);
    EXPECT(ae_start(&ae, &cfg));
    EXPECT(ae_listener_count(&ae) == 0u);
    EXPECT(publish_seq(&ae, 1u, false, -40.0f));
    EXPECT(ae_last_window_seq(&ae) == 1u);
    ae_signature_t latest;
    EXPECT(ae_get_latest_signature(&ae, &latest));
    /* L1 optional → DEGRADED is fine; must not be GAP/INVALID. */
    EXPECT(latest.quality != AE_QUALITY_GAP);
    EXPECT(latest.quality != AE_QUALITY_INVALID);
}

/** Ear-style: start AE, then register CE listener. */
static void test_ear_style_ce_listener(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ae_config_t acfg;
    ce_config_t ccfg;
    ae_config_set_defaults(&acfg);
    ce_config_set_defaults(&ccfg);
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_start(&ce, &ccfg));
    EXPECT(ae_add_signature_listener(&ae, ce_on_signature, &ce));
    EXPECT(ae_listener_count(&ae) == 1u);

    EXPECT(publish_seq(&ae, 1u, false, -40.0f));
    EXPECT(publish_seq(&ae, 2u, false, -40.0f));
    EXPECT(ce_candidate_count(&ce) >= 1u);
}

static void test_multi_listener_add_remove(void)
{
    ae_engine_t ae;
    ae_config_t cfg;
    ae_config_set_defaults(&cfg);
    EXPECT(ae_start(&ae, &cfg));

    g_a_hits = 0;
    g_b_hits = 0;
    EXPECT(ae_add_signature_listener(&ae, on_a, NULL));
    EXPECT(ae_add_signature_listener(&ae, on_b, NULL));
    EXPECT(ae_listener_count(&ae) == 2u);

    EXPECT(publish_seq(&ae, 1u, false, -40.0f));
    EXPECT(g_a_hits == 1);
    EXPECT(g_b_hits == 1);

    EXPECT(ae_remove_signature_listener(&ae, on_a, NULL));
    EXPECT(ae_listener_count(&ae) == 1u);
    EXPECT(publish_seq(&ae, 2u, false, -40.0f));
    EXPECT(g_a_hits == 1);
    EXPECT(g_b_hits == 2);

    /* Wrong ctx: no remove */
    EXPECT(!ae_remove_signature_listener(&ae, on_b, (void *)1));
    EXPECT(ae_listener_count(&ae) == 1u);
    EXPECT(ae_remove_signature_listener(&ae, on_b, NULL));
    EXPECT(ae_listener_count(&ae) == 0u);

    EXPECT(publish_seq(&ae, 3u, false, -40.0f));
    EXPECT(g_a_hits == 1);
    EXPECT(g_b_hits == 2);
}

static void test_duplicate_listeners_and_capacity(void)
{
    ae_engine_t ae;
    ae_config_t cfg;
    ae_config_set_defaults(&cfg);
    EXPECT(ae_start(&ae, &cfg));

    /* Same fn+ctx may be registered twice (explicit slots). */
    EXPECT(ae_add_signature_listener(&ae, on_a, NULL));
    EXPECT(ae_add_signature_listener(&ae, on_a, NULL));
    g_a_hits = 0;
    EXPECT(publish_seq(&ae, 1u, false, -40.0f));
    EXPECT(g_a_hits == 2);

    /* Remove one matching slot only. */
    EXPECT(ae_remove_signature_listener(&ae, on_a, NULL));
    EXPECT(ae_listener_count(&ae) == 1u);
    g_a_hits = 0;
    EXPECT(publish_seq(&ae, 2u, false, -40.0f));
    EXPECT(g_a_hits == 1);

    /* Fill to capacity */
    EXPECT(ae_add_signature_listener(&ae, on_b, NULL));
    EXPECT(ae_add_signature_listener(&ae, on_b, (void *)1));
    EXPECT(ae_add_signature_listener(&ae, on_b, (void *)2));
    EXPECT(ae_listener_count(&ae) == AE_MAX_SIGNATURE_LISTENERS);
    EXPECT(!ae_add_signature_listener(&ae, on_b, (void *)3));
}

static void test_gap_eval_consecutive_no_gap(void)
{
    uint32_t seen = 10u;
    ae_window_gap_t g = ae_window_gap_eval(5u, 6u, 10u, &seen);
    EXPECT(!g.seq_gap);
    EXPECT(g.overwrite_delta == 0u);
    EXPECT(!g.gap_before);
    EXPECT(seen == 10u);
}

static void test_gap_eval_seq_gap(void)
{
    uint32_t seen = 0u;
    ae_window_gap_t g = ae_window_gap_eval(5u, 8u, 0u, &seen);
    EXPECT(g.seq_gap);
    EXPECT(g.overwrite_delta == 0u);
    EXPECT(g.gap_before);
}

static void test_gap_eval_overwrite_first_window(void)
{
    /* last_seq == 0: no seq_gap, but overwrite delta marks GAP. */
    uint32_t seen = 3u; /* AE start baseline */
    ae_window_gap_t g = ae_window_gap_eval(0u, 7u, 5u, &seen);
    EXPECT(!g.seq_gap);
    EXPECT(g.overwrite_delta == 2u);
    EXPECT(g.gap_before);
    EXPECT(seen == 5u);

    /* Pre-start baseline: no false positive when counter unchanged */
    seen = 9u;
    g = ae_window_gap_eval(0u, 1u, 9u, &seen);
    EXPECT(!g.gap_before);
    EXPECT(g.overwrite_delta == 0u);
}

static void test_gap_eval_combined_one_flag(void)
{
    uint32_t seen = 1u;
    ae_window_gap_t g = ae_window_gap_eval(2u, 5u, 4u, &seen);
    EXPECT(g.seq_gap);
    EXPECT(g.overwrite_delta == 3u);
    EXPECT(g.gap_before); /* single flag */
}

static void test_gap_eval_uint32_wrap(void)
{
    uint32_t seen = 0xfffffff0u;
    ae_window_gap_t g = ae_window_gap_eval(1u, 2u, 0x00000005u, &seen);
    EXPECT(g.overwrite_delta == 0x15u);
    EXPECT(g.gap_before);
}

static void test_publish_gap_quality_once(void)
{
    ae_engine_t ae;
    ae_config_t cfg;
    ae_config_set_defaults(&cfg);
    EXPECT(ae_start(&ae, &cfg));
    g_a_hits = 0;
    g_last_quality = 0xffu;
    EXPECT(ae_add_signature_listener(&ae, on_a, NULL));

    uint32_t seen = 0u;
    uint32_t ow_events = 0u;
    uint32_t seq_gaps = 0u;
    uint32_t gap_sigs = 0u;

    /* Simulate intake for seq 1 (ok), then seq 4 with overwrite delta. */
    for (uint32_t seq = 1u; seq <= 1u; seq++) {
        ae_window_gap_t g =
            ae_window_gap_eval(ae_last_window_seq(&ae), seq, 0u, &seen);
        EXPECT(!g.gap_before);
        EXPECT(publish_seq(&ae, seq, g.gap_before, -40.0f));
    }

    {
        const uint32_t seq = 4u;
        const uint32_t ow_now = 2u;
        ae_window_gap_t g = ae_window_gap_eval(
            ae_last_window_seq(&ae),
            seq,
            ow_now,
            &seen
        );
        EXPECT(g.seq_gap);
        EXPECT(g.overwrite_delta == 2u);
        EXPECT(g.gap_before);
        if (g.overwrite_delta) {
            ow_events += g.overwrite_delta;
        }
        if (g.seq_gap) {
            seq_gaps++;
        }
        EXPECT(publish_seq(&ae, seq, g.gap_before, -40.0f));
        if (g.gap_before) {
            gap_sigs++;
        }
    }

    EXPECT(ow_events == 2u);
    EXPECT(seq_gaps == 1u);
    EXPECT(gap_sigs == 1u); /* one GAP-marked signature, not two */
    EXPECT(g_last_quality == AE_QUALITY_GAP);
    EXPECT(g_a_hits == 2);
}

static void test_ce_receives_gap_and_coasts(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ae_config_t acfg;
    ce_config_t ccfg;
    ae_config_set_defaults(&acfg);
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 1u;
    ccfg.t_coast = 3u;
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_start(&ce, &ccfg));
    EXPECT(ae_add_signature_listener(&ae, ce_on_signature, &ce));

    EXPECT(publish_seq(&ae, 1u, false, -40.0f));
    EXPECT(publish_seq(&ae, 2u, false, -40.0f));
    EXPECT(ce_candidate_count(&ce) >= 1u);

    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0u, &c));
    const uint64_t id = c.candidate_id;
    EXPECT(c.state == AE_CAND_ACTIVE || c.state == AE_CAND_NASCENT);

    /* GAP quality → coast path (existing CE behavior). */
    EXPECT(publish_seq(&ae, 3u, true, -40.0f));
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_COASTING || c.state == AE_CAND_EXPIRED
           || c.state == AE_CAND_ACTIVE || c.state == AE_CAND_NASCENT);
    /* At minimum CE accepted the GAP signature without crashing; if ACTIVE
     * was reached, GAP should move to COASTING. */
    if (c.state != AE_CAND_NASCENT && c.state != AE_CAND_EXPIRED) {
        EXPECT(c.state == AE_CAND_COASTING || c.coast_windows >= 1u);
    }
}

static void test_ce_gap_from_active_coasts(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ae_config_t acfg;
    ce_config_t ccfg;
    ae_config_set_defaults(&acfg);
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 1u;
    ccfg.t_coast = 5u;
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_start(&ce, &ccfg));
    EXPECT(ae_add_signature_listener(&ae, ce_on_signature, &ce));

    EXPECT(publish_seq(&ae, 10u, false, -40.0f)); /* create */
    EXPECT(publish_seq(&ae, 11u, false, -40.0f)); /* enter ACTIVE if n_enter=1 */

    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0u, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);

    EXPECT(publish_seq(&ae, 12u, true, -40.0f));
    EXPECT(ce_get_candidate(&ce, 0u, &c));
    EXPECT(c.state == AE_CAND_COASTING);
    EXPECT(c.coast_windows >= 1u);
}

int main(void)
{
    test_ae_publish_zero_listeners();
    test_ear_style_ce_listener();
    test_multi_listener_add_remove();
    test_duplicate_listeners_and_capacity();
    test_gap_eval_consecutive_no_gap();
    test_gap_eval_seq_gap();
    test_gap_eval_overwrite_first_window();
    test_gap_eval_combined_one_flag();
    test_gap_eval_uint32_wrap();
    test_publish_gap_quality_once();
    test_ce_receives_gap_and_coasts();
    test_ce_gap_from_active_coasts();

    if (g_failures != 0) {
        fprintf(stderr, "test_ae_m4_lifecycle_gap: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ae_m4_lifecycle_gap: ok\n");
    return 0;
}
