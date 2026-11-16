/**
 * Host tests: AS-001 acoustic profile load + match (provisional).
 *
 * Uses heli_peak / bg_pre feature values from
 * docs/acoustic-profiles/AS-001/artifacts/signatures.csv (real AE extract).
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
 *     firmware/products/dbmap/ear/acoustic_profile/test_host/test_as_profile_as001.c \
 *     -lm -o /tmp/test_as_profile_as001 && /tmp/test_as_profile_as001
 */
#include "ae_engine.h"
#include "ae_signature.h"
#include "ae_types.h"
#include "as_profile.h"
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

/** signatures.csv heli_peak seq=17 */
static void sig_heli_peak_17(ae_signature_t *s)
{
    fill_base(s, 17);
    s->rms_dbfs = -9.4500f;
    s->peak_dbfs = -2.1124f;
    s->crest_db = 7.3376f;
    s->band_dbfs[0] = -17.6648f;
    s->band_dbfs[1] = -16.8075f;
    s->band_dbfs[2] = -26.0515f;
    s->band_dbfs[3] = -48.4829f;
    s->hl_db = -11.8221f;
    s->hl_valid = true;
    s->spectral_valid = true;
    s->centroid_hz = 1053.7648f;
    s->bandwidth_hz = 1244.6664f;
    s->dominant_hz = 500.0000f;
    s->flatness = 0.246292f;
    s->dynamics_valid = true;
    s->spectral_flux = 1.013326f;
    s->envelope_cv = 0.121658f;
}

/** signatures.csv bg_pre seq=1 */
static void sig_bg_pre_1(ae_signature_t *s)
{
    fill_base(s, 1);
    s->rms_dbfs = -33.9854f;
    s->peak_dbfs = -22.7265f;
    s->crest_db = 11.2589f;
    s->band_dbfs[0] = -36.0956f;
    s->band_dbfs[1] = -40.5246f;
    s->band_dbfs[2] = -47.2211f;
    s->band_dbfs[3] = -52.5987f;
    s->hl_db = -11.3575f;
    s->hl_valid = true;
    s->spectral_valid = true;
    s->centroid_hz = 1840.5762f;
    s->bandwidth_hz = 1949.1804f;
    s->dominant_hz = 93.7500f;
    s->flatness = 0.601279f;
    s->dynamics_valid = true;
    s->spectral_flux = 0.995683f;
    s->envelope_cv = 0.085697f;
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
    EXPECT(!p->uses_pair);
    EXPECT(p->ae_schema_version == AE_SIGNATURE_SCHEMA_VERSION);
}

static void test_heli_peak_matches(void)
{
    ae_signature_t sig;
    sig_heli_peak_17(&sig);

    as_profile_match_result_t r;
    EXPECT(as_profile_match(as_profile_as001(), &sig, &r));
    EXPECT(r.matched);
    EXPECT(r.profile_id == AS_PROFILE_ID_AS001);
    EXPECT(r.acoustic_class == AS_CLASS_HELICOPTER_LIKE);
    EXPECT(r.distance <= as_profile_as001()->match_distance_max);
    EXPECT(r.skipped_pair == 1u); /* profile does not use pair */
    EXPECT(r.skipped_spectral == 0u);
}

static void test_background_does_not_match(void)
{
    ae_signature_t sig;
    sig_bg_pre_1(&sig);

    as_profile_match_result_t r;
    EXPECT(!as_profile_match(as_profile_as001(), &sig, &r));
    EXPECT(!r.matched);
}

static void test_missing_pair_not_compared(void)
{
    ae_signature_t sig;
    sig_heli_peak_17(&sig);
    /* Even if pair_valid were spuriously set, uses_pair=false skips it. */
    sig.pair_valid = true;
    sig.delta_rms_db = 99.0f;
    sig.flags = 0u;

    as_profile_match_result_t r;
    EXPECT(as_profile_match(as_profile_as001(), &sig, &r));
    EXPECT(r.matched);
    EXPECT(r.skipped_pair == 1u);

    /* Mono path: pair missing must still match peak. */
    sig.pair_valid = false;
    sig.flags = AE_FLAG_PAIR_MISSING;
    EXPECT(as_profile_match(as_profile_as001(), &sig, &r));
    EXPECT(r.matched);
    EXPECT(r.skipped_pair == 1u);
}

static void test_unavailable_spectral_safe(void)
{
    ae_signature_t sig;
    sig_heli_peak_17(&sig);
    sig.spectral_valid = false;
    sig.dominant_hz = 0.0f;
    sig.flatness = 0.0f;
    sig.centroid_hz = 0.0f;
    sig.flags |= AE_FLAG_L1_MISSING;

    as_profile_match_result_t r;
    EXPECT(!as_profile_match(as_profile_as001(), &sig, &r));
    EXPECT(!r.matched);
    EXPECT(r.skipped_spectral == 1u);
    EXPECT(r.rejected_eligibility == 1u);
}

static void test_workflow_classifies_without_promote(void)
{
    ae_engine_t ae;
    EXPECT(ae_start(&ae, NULL));
    ce_engine_t ce;
    ce_config_t cfg;
    ce_config_set_defaults(&cfg);
    cfg.n_enter = 1u;
    EXPECT(ce_start(&ce, &cfg));
    EXPECT(ce_bind_audio_engine(&ce, &ae));

    ae_signature_t peak;
    sig_heli_peak_17(&peak);
    EXPECT(ae_publish_signature(&ae, &peak));

    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0, &c));
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

    ae_signature_t bg;
    sig_bg_pre_1(&bg);
    bg.window_seq = 18;
    bg.window_end_mono_us = 18000000;
    EXPECT(ae_publish_signature(&ae, &bg));

    ce_get_diag(&ce, &d);
    EXPECT(d.profile_match_misses >= 1u);

    ce_stop(&ce);
}

int main(void)
{
    test_registry_loads_as001();
    test_heli_peak_matches();
    test_background_does_not_match();
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
