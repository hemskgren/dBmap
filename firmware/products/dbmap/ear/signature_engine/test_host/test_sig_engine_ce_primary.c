/**
 * F4: Production-shaped engine-before-CE integration.
 *
 * Registers Signature Engine AE listener, then binds CE to the same AE.
 * Publishes a heli window and verifies CE annotation uses engine primary
 * (same window_seq), not the legacy as_profile_match_best-only path.
 *
 * Gate: firmware/scripts/run_firmware_fast_gate.sh (sig_engine_ce_primary)
 */
#include "ae_engine.h"
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

static void fill_heli(ae_signature_t *s, uint32_t seq, float rms_dbfs)
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

static void fill_broadband(ae_signature_t *s, uint32_t seq, float rms_dbfs)
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

int main(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ae_config_t acfg;
    ce_config_t ccfg;

    ae_config_set_defaults(&acfg);
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 1u;

    EXPECT(ae_start(&ae, &acfg));
    EXPECT(sig_engine_start());
    EXPECT(sig_engine_is_started());

    /* Production order: engine listener before CE bind. */
    EXPECT(ae_add_signature_listener(&ae, sig_engine_ae_listener, NULL));
    EXPECT(ae_listener_count(&ae) == 1u);

    EXPECT(ce_start(&ce, &ccfg));
    EXPECT(ce_bind_audio_engine(&ce, &ae));
    EXPECT(ae_listener_count(&ae) == 2u);

    ae_signature_t sig;
    for (uint32_t seq = 1u; seq <= 4u; seq++) {
        fill_broadband(&sig, seq, -55.0f);
        EXPECT(ae_publish_signature(&ae, &sig));
    }

    const uint32_t seq = 17u;
    fill_heli(&sig, seq, -40.0f);
    EXPECT(ae_publish_signature(&ae, &sig));

    sig_primary_distance_t prim;
    EXPECT(sig_engine_get_primary_distance_match(&prim));
    EXPECT(prim.valid);
    EXPECT(prim.window_seq == seq);
    EXPECT(prim.profile_id == AS_PROFILE_ID_AS001);

    /* Find the candidate that absorbed the heli window. */
    ae_candidate_t cand;
    bool found = false;
    for (uint8_t i = 0u; i < CE_MAX_CANDIDATES; i++) {
        if (!ce_get_candidate(&ce, i, &cand)) {
            continue;
        }
        if (cand.last_window_seq == seq) {
            found = true;
            break;
        }
    }
    EXPECT(found);

    ce_classification_t cls;
    EXPECT(ce_get_classification(&ce, cand.candidate_id, &cls));
    EXPECT(cls.valid);
    EXPECT(cls.matched);
    EXPECT(cls.profile_id == AS_PROFILE_ID_AS001);
    EXPECT(cls.profile_id == prim.profile_id);
    /* Engine path: distance annotation matches published primary. */
    EXPECT(cls.distance == prim.distance);

    /*
     * If CE had fallen back to as_profile_match_best while engine was started
     * with a stale/empty primary, matched would still be true for this sig —
     * the window_seq gate + distance equality above pin the engine path.
     * Also confirm engine remains started (fallback only when not started).
     */
    EXPECT(sig_engine_is_started());

    ce_stop(&ce);
    sig_engine_stop();

    if (g_failures != 0) {
        fprintf(stderr, "test_sig_engine_ce_primary: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_sig_engine_ce_primary: ok\n");
    return 0;
}
