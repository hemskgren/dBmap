/**
 * Host tests: AS-001 acoustic profile load + match (provisional).
 *
 * Uses BBC / device-bench helicopter feature centers (relative-bg eligibility).
 *
 * From repo root:
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/acoustic_profile/include \
 *     -I firmware/products/dbmap/ear/candidate_engine/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     -I firmware/products/dbmap/ear/signature_engine/include \
 *     -I firmware/products/dbmap/ear/ear_observation/include \
 *     -I firmware/platform/audio/include \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_history.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_time.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_engine.c \
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
 *     firmware/products/dbmap/ear/acoustic_profile/test_host/test_as_profile_as001.c \
 *     -lm -o /tmp/test_as_profile_as001 && /tmp/test_as_profile_as001
 */
#include "ae_engine.h"
#include "ae_signature.h"
#include "ae_types.h"
#include "as_profile.h"
#include "ce_engine.h"
#include "sig_engine.h"

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

static void fill_base(ae_signature_t *s, uint32_t seq)
{
    memset(s, 0, sizeof(*s));
    s->schema_version = AE_SIGNATURE_SCHEMA_VERSION;
    s->window_seq = seq;
    s->window_end_mono_us = (int64_t)seq * 1000000;
    s->window_ms = 1000;
    s->mic_mask = 1u;
    s->quality = AE_QUALITY_OK;
    s->flags = AE_FLAG_PAIR_MISSING;
    s->pair_valid = false;
}

/** BBC / device-bench heli shape at a chosen absolute RMS. */
static void sig_heli(ae_signature_t *s, uint32_t seq, float rms_dbfs)
{
    fill_base(s, seq);
    s->rms_dbfs = rms_dbfs;
    s->peak_dbfs = rms_dbfs + 12.0f;
    s->crest_db = 12.0f;
    s->band_dbfs[0] = rms_dbfs - 4.2094f;
    s->band_dbfs[1] = rms_dbfs - 3.5240f;
    s->band_dbfs[2] = rms_dbfs - 13.0958f;
    s->band_dbfs[3] = rms_dbfs - 32.7691f;
    s->hl_db = -12.0f;
    s->hl_valid = true;
    s->spectral_valid = true;
    s->centroid_hz = 800.0f;
    s->bandwidth_hz = 920.0f;
    s->dominant_hz = 230.0f;
    s->flatness = 0.1410f;
    s->dynamics_valid = true;
}

/** Office-like broadband background. */
static void sig_bg(ae_signature_t *s, uint32_t seq, float rms_dbfs)
{
    fill_base(s, seq);
    s->rms_dbfs = rms_dbfs;
    s->peak_dbfs = rms_dbfs + 11.0f;
    s->crest_db = 11.0f;
    s->band_dbfs[0] = rms_dbfs - 2.0f;
    s->band_dbfs[1] = rms_dbfs - 1.0f;
    s->band_dbfs[2] = rms_dbfs - 1.5f;
    s->band_dbfs[3] = rms_dbfs - 3.0f;
    s->hl_db = -11.0f;
    s->hl_valid = true;
    s->spectral_valid = true;
    s->centroid_hz = 1800.0f;
    s->bandwidth_hz = 1900.0f;
    s->dominant_hz = 125.0f;
    s->flatness = 0.75f;
    s->dynamics_valid = true;
}

static void seed_bg(as_profile_bg_state_t *bg, float rms_dbfs)
{
    as_profile_bg_init(bg);
    ae_signature_t s;
    as_profile_match_result_t r;
    for (uint32_t i = 1u; i <= 4u; i++) {
        sig_bg(&s, i, rms_dbfs);
        EXPECT(!as_profile_match_with_bg(as_profile_as001(), &s, bg, &r));
    }
    EXPECT(bg->bg_valid);
}

static void test_registry_loads_as001(void)
{
    EXPECT(as_profile_count() >= 1u);
    const as_profile_t *p = as_profile_get_by_id(AS_PROFILE_ID_AS001);
    EXPECT(p != NULL);
    EXPECT(p == as_profile_as001());
    EXPECT(p->acoustic_class == AS_CLASS_HELICOPTER_LIKE);
    EXPECT(p->provisional);
    EXPECT(p->uses_spectral);
    EXPECT(p->uses_rms_rel_bg);
    EXPECT(!p->uses_rms);
    EXPECT(!p->uses_pair);
    EXPECT(p->ae_schema_version == AE_SIGNATURE_SCHEMA_VERSION);
    EXPECT(p->rms_bg_margin_db == 3.5f);
}

static void test_heli_matches_with_relative_bg(void)
{
    as_profile_bg_state_t bg;
    seed_bg(&bg, -52.0f);

    ae_signature_t sig;
    sig_heli(&sig, 10u, -38.0f); /* ~14 dB above bg */

    as_profile_match_result_t r;
    EXPECT(as_profile_match_with_bg(as_profile_as001(), &sig, &bg, &r));
    EXPECT(r.matched);
    EXPECT(r.profile_id == AS_PROFILE_ID_AS001);
    EXPECT(r.acoustic_class == AS_CLASS_HELICOPTER_LIKE);
    EXPECT(r.distance <= as_profile_as001()->match_distance_max);
    EXPECT(r.skipped_pair == 1u);
    EXPECT(r.skipped_spectral == 0u);

    /* Stateless API rejects relative-bg profiles. */
    EXPECT(!as_profile_match(as_profile_as001(), &sig, &r));
    EXPECT(r.rejected_eligibility == 1u);
}

static void test_background_does_not_match(void)
{
    as_profile_bg_state_t bg;
    seed_bg(&bg, -52.0f);

    ae_signature_t sig;
    sig_bg(&sig, 10u, -51.0f);

    as_profile_match_result_t r;
    EXPECT(!as_profile_match_with_bg(as_profile_as001(), &sig, &bg, &r));
    EXPECT(!r.matched);
}

static void test_below_margin_rejected(void)
{
    as_profile_bg_state_t bg;
    seed_bg(&bg, -52.0f);

    ae_signature_t sig;
    sig_heli(&sig, 10u, -50.0f); /* only +2 dB vs bg, margin 3.5 */

    as_profile_match_result_t r;
    EXPECT(!as_profile_match_with_bg(as_profile_as001(), &sig, &bg, &r));
    EXPECT(r.rejected_eligibility == 1u);
}

static void test_missing_pair_not_compared(void)
{
    as_profile_bg_state_t bg;
    seed_bg(&bg, -52.0f);

    ae_signature_t sig;
    sig_heli(&sig, 10u, -38.0f);
    sig.pair_valid = true;
    sig.delta_rms_db = 99.0f;
    sig.flags = 0u;

    as_profile_match_result_t r;
    EXPECT(as_profile_match_with_bg(as_profile_as001(), &sig, &bg, &r));
    EXPECT(r.matched);
    EXPECT(r.skipped_pair == 1u);

    sig.pair_valid = false;
    sig.flags = AE_FLAG_PAIR_MISSING;
    EXPECT(as_profile_match_with_bg(as_profile_as001(), &sig, &bg, &r));
    EXPECT(r.matched);
}

static void test_unavailable_spectral_safe(void)
{
    as_profile_bg_state_t bg;
    seed_bg(&bg, -52.0f);

    ae_signature_t sig;
    sig_heli(&sig, 10u, -38.0f);
    sig.spectral_valid = false;
    sig.dominant_hz = 0.0f;
    sig.flatness = 0.0f;
    sig.centroid_hz = 0.0f;
    sig.flags |= AE_FLAG_L1_MISSING;

    as_profile_match_result_t r;
    EXPECT(!as_profile_match_with_bg(as_profile_as001(), &sig, &bg, &r));
    EXPECT(!r.matched);
    EXPECT(r.skipped_spectral == 1u);
    EXPECT(r.rejected_eligibility == 1u);
}

static void test_workflow_classifies_without_promote(void)
{
    EXPECT(sig_engine_start());

    ae_engine_t ae;
    EXPECT(ae_start(&ae, NULL));
    /* Production order: Signature Engine listens before CE bind. */
    EXPECT(ae_add_signature_listener(&ae, sig_engine_ae_listener, NULL));

    ce_engine_t ce;
    ce_config_t cfg;
    ce_config_set_defaults(&cfg);
    cfg.n_enter = 1u;
    EXPECT(ce_start(&ce, &cfg));
    EXPECT(ce_bind_audio_engine(&ce, &ae));

    ae_signature_t win;
    /* Seed relative bg through the live Signature Engine. */
    for (uint32_t seq = 1u; seq <= 4u; seq++) {
        sig_bg(&win, seq, -52.0f);
        EXPECT(ae_publish_signature(&ae, &win));
    }

    sig_heli(&win, 5u, -38.0f);
    EXPECT(ae_publish_signature(&ae, &win));

    ae_candidate_t c;
    bool found = false;
    for (uint8_t i = 0u; i < 8u; i++) {
        if (!ce_get_candidate(&ce, i, &c)) {
            continue;
        }
        if (c.last_window_seq == 5u) {
            found = true;
            break;
        }
    }
    EXPECT(found);
    EXPECT(c.state != AE_CAND_PROMOTED);

    ce_classification_t cls;
    EXPECT(ce_get_classification(&ce, c.candidate_id, &cls));
    EXPECT(cls.matched);
    EXPECT(cls.profile_id == AS_PROFILE_ID_AS001);
    EXPECT(cls.acoustic_class == AS_CLASS_HELICOPTER_LIKE);

    ce_diag_t d;
    ce_get_diag(&ce, &d);
    EXPECT(d.profile_match_hits >= 1u);
    EXPECT(d.promote_successes == 0u);

    sig_bg(&win, 6u, -52.0f);
    EXPECT(ae_publish_signature(&ae, &win));

    sig_engine_stop();
}

int main(void)
{
    test_registry_loads_as001();
    test_heli_matches_with_relative_bg();
    test_background_does_not_match();
    test_below_margin_rejected();
    test_missing_pair_not_compared();
    test_unavailable_spectral_safe();
    test_workflow_classifies_without_promote();

    if (g_failures != 0) {
        fprintf(stderr, "test_as_profile_as001: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_as_profile_as001: ok\n");
    return 0;
}
