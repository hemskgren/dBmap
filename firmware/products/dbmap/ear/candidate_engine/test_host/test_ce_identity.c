/**
 * Host tests: Candidate Engine Stage 1 identity quality.
 *
 * Deterministic synthetic signatures only — not real-world source separation.
 *
 * From repo root:
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -g \
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
 *     firmware/products/dbmap/ear/candidate_engine/test_host/test_ce_identity.c \
 *     -lm -o /tmp/test_ce_identity && /tmp/test_ce_identity
 */
#include "ae_engine.h"
#include "ae_signature.h"
#include "ae_types.h"
#include "ce_engine.h"
#include "ce_manager.h"

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
    float rms_dbfs;
    float band_rel[AE_BAND_COUNT]; /* band_dbfs[i] - rms */
    bool spectral_valid;
    float dominant_hz;
    float flatness;
    float centroid_hz;
    bool pair_valid;
    float delta_rms_db;
    bool gap;
} synth_feat_t;

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

static void default_band_rel(float out[AE_BAND_COUNT])
{
    out[0] = -2.0f;
    out[1] = 0.0f;
    out[2] = -1.0f;
    out[3] = -4.0f;
}

static bool publish_synth(
    ae_engine_t *ae,
    uint32_t seq,
    const synth_feat_t *f
)
{
    dbmap_l0_window_t l0;
    fill_l0_shape(&l0, f->rms_dbfs, f->band_rel, (int64_t)seq * 1000000);

    dbmap_l1_window_t l1;
    dbmap_l1_window_t *l1p = NULL;
    memset(&l1, 0, sizeof(l1));
    if (f->spectral_valid) {
        l1.valid = true;
        l1.clipped = false;
        l1.window_end_mono_us = l0.window_end_mono_us;
        l1.window_ms = 1000;
        l1.sample_count = 16000;
        l1.centroid_hz = f->centroid_hz;
        l1.bandwidth_hz = 200.0f;
        l1.dominant_hz = f->dominant_hz;
        l1.flatness = f->flatness;
        l1p = &l1;
    }

    ae_pair_metrics_t pair;
    ae_pair_metrics_t *pairp = NULL;
    memset(&pair, 0, sizeof(pair));
    if (f->pair_valid) {
        pair.valid = true;
        pair.mic0_rms_dbfs = f->rms_dbfs;
        pair.mic1_rms_dbfs = f->rms_dbfs - f->delta_rms_db;
        pair.mic0_peak_dbfs = f->rms_dbfs + 6.0f;
        pair.mic1_peak_dbfs = pair.mic1_rms_dbfs + 6.0f;
        pair.mic_mask = 0x03u;
        pair.stereo_degraded = false;
        pairp = &pair;
    }

    ae_signature_t sig;
    if (!ae_signature_build(seq, &l0, l1p, NULL, pairp, f->gap, &sig)) {
        return false;
    }
    return ae_publish_signature(ae, &sig);
}

static void setup_ae_ce(ae_engine_t *ae, ce_engine_t *ce, ce_config_t *cfg_out)
{
    ae_config_t acfg;
    ce_config_t ccfg;
    ae_config_set_defaults(&acfg);
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 2u;
    ccfg.t_coast = 3u;
    EXPECT(ae_start(ae, &acfg));
    EXPECT(ce_start(ce, &ccfg));
    EXPECT(ae_add_signature_listener(ae, ce_on_signature, ce));
    if (cfg_out != NULL) {
        *cfg_out = ccfg;
    }
}

static synth_feat_t tone_a(float rms)
{
    synth_feat_t f;
    memset(&f, 0, sizeof(f));
    f.rms_dbfs = rms;
    default_band_rel(f.band_rel);
    f.spectral_valid = true;
    f.dominant_hz = 200.0f;
    f.flatness = 0.20f;
    f.centroid_hz = 250.0f;
    return f;
}

static synth_feat_t tone_b(float rms)
{
    synth_feat_t f;
    memset(&f, 0, sizeof(f));
    f.rms_dbfs = rms;
    f.band_rel[0] = -6.0f;
    f.band_rel[1] = -4.0f;
    f.band_rel[2] = 0.0f;
    f.band_rel[3] = 2.0f;
    f.spectral_valid = true;
    f.dominant_hz = 1200.0f;
    f.flatness = 0.70f;
    f.centroid_hz = 1400.0f;
    return f;
}

/** 1. Evolving level keeps the same candidate_id. */
static void test_level_change_same_id(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL);

    synth_feat_t f = tone_a(-40.0f);
    EXPECT(publish_synth(&ae, 1u, &f));
    EXPECT(publish_synth(&ae, 2u, &f));
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    const uint64_t id = c.candidate_id;

    f = tone_a(-55.0f);
    EXPECT(publish_synth(&ae, 3u, &f));
    f = tone_a(-30.0f);
    EXPECT(publish_synth(&ae, 4u, &f));
    f = tone_a(-48.0f);
    EXPECT(publish_synth(&ae, 5u, &f));

    EXPECT(ce_candidate_count(&ce) == 1u);
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.candidate_id == id);
    EXPECT(c.state == AE_CAND_ACTIVE);
}

/** 2. Two distinct spectral signatures maintain two candidates. */
static void test_two_spectral_candidates(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL);

    synth_feat_t a = tone_a(-40.0f);
    synth_feat_t b = tone_b(-40.0f);
    EXPECT(publish_synth(&ae, 1u, &a));
    EXPECT(publish_synth(&ae, 2u, &b));
    EXPECT(ce_candidate_count(&ce) == 2u);

    EXPECT(publish_synth(&ae, 3u, &a));
    EXPECT(publish_synth(&ae, 4u, &b));
    EXPECT(publish_synth(&ae, 5u, &a));
    EXPECT(publish_synth(&ae, 6u, &b));
    EXPECT(ce_candidate_count(&ce) == 2u);

    uint8_t active = 0u;
    for (uint8_t i = 0; i < 2u; i++) {
        ae_candidate_t c;
        EXPECT(ce_get_candidate(&ce, i, &c));
        if (c.state == AE_CAND_ACTIVE) {
            active++;
        }
    }
    EXPECT(active == 2u);
}

/**
 * 3. Invalid/unavailable spectral/pair must not distort matching via zeros.
 * Candidate built without spectral; later spectral-bearing same-shape sig still
 * associates (spectral terms skipped until both sides valid).
 */
static void test_missing_spectral_no_zero_distortion(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL);

    synth_feat_t plain;
    memset(&plain, 0, sizeof(plain));
    plain.rms_dbfs = -40.0f;
    default_band_rel(plain.band_rel);
    plain.spectral_valid = false;
    plain.pair_valid = false;

    EXPECT(publish_synth(&ae, 1u, &plain));
    EXPECT(publish_synth(&ae, 2u, &plain));
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(!c.spectral_ema_valid);
    EXPECT(!c.pair_ema_valid);
    const uint64_t id = c.candidate_id;

    /* Same shape + spectral present: must rematch, not create via zero-vs-200Hz. */
    synth_feat_t with_spec = tone_a(-40.0f);
    EXPECT(publish_synth(&ae, 3u, &with_spec));
    EXPECT(ce_candidate_count(&ce) == 1u);
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.spectral_ema_valid);

    /* Drop spectral again — still rematch on bands/RMS only. */
    EXPECT(publish_synth(&ae, 4u, &plain));
    EXPECT(ce_candidate_count(&ce) == 1u);
    EXPECT(ce_find_candidate(&ce, id, &c));
}

static void restart_ce_with_cfg(ae_engine_t *ae, ce_engine_t *ce, ce_config_t *cfg)
{
    EXPECT(ae_remove_signature_listener(ae, ce_on_signature, ce));
    ce_stop(ce);
    EXPECT(ce_start(ce, cfg));
    EXPECT(ae_add_signature_listener(ae, ce_on_signature, ce));
}

/** 4. Valid pair features contribute: distinct ΔRMS can spawn a second track. */
static void test_pair_features_contribute(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ce_config_t cfg;
    setup_ae_ce(&ae, &ce, &cfg);
    /* Tighter match so pair term alone can exceed rematch. */
    cfg.match_distance_max = 2.0f;
    cfg.match_hysteresis = 0.5f;
    restart_ce_with_cfg(&ae, &ce, &cfg);

    synth_feat_t left = tone_a(-40.0f);
    left.pair_valid = true;
    left.delta_rms_db = 0.0f;
    EXPECT(publish_synth(&ae, 1u, &left));
    EXPECT(publish_synth(&ae, 2u, &left));
    EXPECT(ce_candidate_count(&ce) == 1u);

    synth_feat_t right = tone_a(-40.0f);
    right.pair_valid = true;
    right.delta_rms_db = 12.0f; /* 0.25 * 12 = 3 > match_max+hyst */
    EXPECT(publish_synth(&ae, 3u, &right));
    EXPECT(ce_candidate_count(&ce) == 2u);
}

/** 5. GAP coast then rematch restores the same candidate_id. */
static void test_gap_rematch_same_id(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL);

    synth_feat_t f = tone_a(-40.0f);
    EXPECT(publish_synth(&ae, 1u, &f));
    EXPECT(publish_synth(&ae, 2u, &f));
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0, &c));
    const uint64_t id = c.candidate_id;

    f.gap = true;
    EXPECT(publish_synth(&ae, 3u, &f));
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_COASTING);

    f.gap = false;
    EXPECT(publish_synth(&ae, 4u, &f));
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    EXPECT(c.candidate_id == id);
    EXPECT(ce_candidate_count(&ce) == 1u);
}

/**
 * 6. Hysteresis prevents duplicate creation near the match boundary.
 * Mild band-shape nudge lands in (match_max, match_max+hyst].
 */
static void test_hysteresis_suppresses_duplicate(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ce_config_t cfg;
    setup_ae_ce(&ae, &ce, &cfg);
    cfg.match_distance_max = 1.0f;
    cfg.match_hysteresis = 2.0f;
    restart_ce_with_cfg(&ae, &ce, &cfg);

    synth_feat_t f = tone_a(-40.0f);
    f.spectral_valid = false; /* band-shape only */
    EXPECT(publish_synth(&ae, 1u, &f));
    EXPECT(publish_synth(&ae, 2u, &f));
    EXPECT(ce_candidate_count(&ce) == 1u);

    /*
     * Relative band nudge: Δshape on one band of 4.0 → 0.30*4 = 1.2
     * which is in (1.0, 3.0] → hysteresis hold, no second candidate.
     */
    f.band_rel[1] = 4.0f;
    EXPECT(publish_synth(&ae, 3u, &f));
    EXPECT(ce_candidate_count(&ce) == 1u);

    ce_diag_t d;
    ce_get_diag(&ce, &d);
    EXPECT(d.match_hysteresis_hold >= 1u);
    EXPECT(d.created == 1u);
}

/** 7. Genuinely distinct concurrent signature still creates a second candidate. */
static void test_distinct_still_creates(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL);

    synth_feat_t a = tone_a(-40.0f);
    EXPECT(publish_synth(&ae, 1u, &a));
    EXPECT(publish_synth(&ae, 2u, &a));
    EXPECT(ce_candidate_count(&ce) == 1u);

    synth_feat_t b = tone_b(-40.0f);
    EXPECT(publish_synth(&ae, 3u, &b));
    EXPECT(ce_candidate_count(&ce) == 2u);
}

/**
 * 8. Ambiguous signature updates at most one candidate; no unjustified third.
 * Plant two close ACTIVE tracks (create path would merge them), then ingest
 * a midpoint signature.
 */
static void test_ambiguous_updates_one(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ce_config_t cfg;
    setup_ae_ce(&ae, &ce, &cfg);
    cfg.match_distance_max = 8.0f;
    cfg.match_ambiguity_delta = 8.0f;
    cfg.match_hysteresis = 0.0f;
    restart_ce_with_cfg(&ae, &ce, &cfg);

    float bands[AE_BAND_COUNT];
    default_band_rel(bands);

    for (uint8_t i = 0; i < 2u; i++) {
        ae_candidate_t *c = &ce.slots[i];
        memset(c, 0, sizeof(*c));
        c->in_use = true;
        c->candidate_id = (uint64_t)(i + 1u);
        c->state = AE_CAND_ACTIVE;
        c->evidence_count = 3u;
        c->ema_rms_dbfs = -40.0f;
        for (uint32_t b = 0; b < AE_BAND_COUNT; b++) {
            c->band_ema[b] = -40.0f + bands[b];
        }
        c->spectral_ema_valid = true;
        c->ema_dominant_hz = (i == 0u) ? 400.0f : 480.0f;
        c->ema_flatness = (i == 0u) ? 0.40f : 0.42f;
        c->ema_centroid_hz = (i == 0u) ? 450.0f : 500.0f;
        c->quality_summary = AE_QUALITY_OK;
    }
    ce.next_id = 3u;

    EXPECT(ce_candidate_count(&ce) == 2u);

    synth_feat_t mid = tone_a(-40.0f);
    mid.dominant_hz = 440.0f;
    mid.flatness = 0.41f;
    mid.centroid_hz = 475.0f;
    EXPECT(publish_synth(&ae, 1u, &mid));

    EXPECT(ce_candidate_count(&ce) == 2u);
    ce_diag_t d;
    ce_get_diag(&ce, &d);
    EXPECT(d.created == 0u);
    EXPECT(d.match_ambiguous >= 1u);
    EXPECT(d.associated >= 1u);
}

/** Promote contract still works after identity changes. */
static void test_promote_still_works(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    setup_ae_ce(&ae, &ce, NULL);

    synth_feat_t f = tone_a(-40.0f);
    EXPECT(publish_synth(&ae, 1u, &f));
    EXPECT(publish_synth(&ae, 2u, &f));
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(ce_promote(&ce, c.candidate_id));
    EXPECT(ce_find_candidate(&ce, c.candidate_id, &c));
    EXPECT(c.state == AE_CAND_PROMOTED);
}

int main(void)
{
    test_level_change_same_id();
    test_two_spectral_candidates();
    test_missing_spectral_no_zero_distortion();
    test_pair_features_contribute();
    test_gap_rematch_same_id();
    test_hysteresis_suppresses_duplicate();
    test_distinct_still_creates();
    test_ambiguous_updates_one();
    test_promote_still_works();

    if (g_failures != 0) {
        fprintf(stderr, "test_ce_identity: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ce_identity: ok\n");
    return 0;
}
