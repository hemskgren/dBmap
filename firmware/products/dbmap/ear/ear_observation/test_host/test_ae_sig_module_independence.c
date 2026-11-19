/**
 * Sprint 2 host tests: AE → Acoustic Signature module independence.
 *
 * Proves production AE multi-listener fan-out lets CE (as.profile / AS-001) and
 * the AS-002 siren detector consume the same published signatures without:
 *   - requiring RMS activity START / activity_detector involvement
 *   - removing shared AE history for each other
 *   - treating GAP/INVALID as silent valid absence
 *
 * Does not exercise Observation Manager create/promote policy redesign.
 *
 * Gate: firmware/scripts/run_firmware_fast_gate.sh (ae_sig_module_independence)
 */
#include "ae_engine.h"
#include "ae_signature.h"
#include "as_profile.h"
#include "ce_engine.h"
#include "ear_siren_se_public.h"

#include <math.h>
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

/** Product-style AE listener shim (mirrors ear_observation siren_on_ae_signature). */
static void siren_ae_listener(const ae_signature_t *sig, void *ctx)
{
    ear_siren_se_public_on_signature((ear_siren_se_public_t *)ctx, sig);
}

static void fill_base(ae_signature_t *s, uint32_t seq, ae_quality_t q)
{
    memset(s, 0, sizeof(*s));
    s->schema_version = AE_SIGNATURE_SCHEMA_VERSION;
    s->window_seq = seq;
    s->window_end_mono_us = (int64_t)seq * 1000000LL;
    s->window_ms = 1000u;
    s->mic_mask = 0x1u;
    s->quality = (uint8_t)q;
    s->flags = AE_FLAG_PAIR_MISSING;
}

/** AS-001 heli_peak-like (signatures.csv seq 17), adjustable seq. */
static void fill_heli(ae_signature_t *s, uint32_t seq)
{
    fill_base(s, seq, AE_QUALITY_OK);
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
}

/** Quiet broadband-ish window (below typical RMS ENTER −45 dBFS). */
static void fill_quiet_bg(ae_signature_t *s, uint32_t seq)
{
    fill_base(s, seq, AE_QUALITY_OK);
    s->rms_dbfs = -55.0f;
    s->peak_dbfs = -48.0f;
    s->crest_db = 7.0f;
    s->band_dbfs[0] = -58.0f;
    s->band_dbfs[1] = -56.0f;
    s->band_dbfs[2] = -60.0f;
    s->band_dbfs[3] = -65.0f;
    s->hl_valid = true;
    s->spectral_valid = true;
    s->centroid_hz = 1800.0f;
    s->bandwidth_hz = 2000.0f;
    s->dominant_hz = 1200.0f;
    s->flatness = 0.70f;
}

/** Siren-like tonal window (AS-002 detector path). */
static void fill_siren(ae_signature_t *s, uint32_t seq)
{
    fill_base(s, seq, AE_QUALITY_OK);
    s->rms_dbfs = -31.0f;
    s->peak_dbfs = -24.0f;
    s->crest_db = 7.0f;
    s->band_dbfs[0] = -34.0f;
    s->band_dbfs[1] = -31.0f;
    s->band_dbfs[2] = -37.0f;
    s->band_dbfs[3] = -43.0f;
    s->hl_valid = true;
    s->spectral_valid = true;
    s->centroid_hz = 400.0f;
    s->bandwidth_hz = 80.0f;
    s->dominant_hz = 344.0f;
    s->flatness = 0.28f;
}

/**
 * A: Signature analysis without RMS START / activity detector.
 * Quiet windows still publish to CE; no activity_detector is linked.
 */
static void test_sig_path_without_rms_start(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ae_config_t acfg;
    ce_config_t ccfg;
    ae_config_set_defaults(&acfg);
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 1u;
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_start(&ce, &ccfg));
    EXPECT(ce_bind_audio_engine(&ce, &ae));

    /* Below typical RMS ENTER (−45): activity would stay IDLE if present. */
    for (uint32_t seq = 1u; seq <= 3u; seq++) {
        ae_signature_t sig;
        fill_quiet_bg(&sig, seq);
        EXPECT(ae_publish_signature(&ae, &sig));
    }
    EXPECT(ae_last_window_seq(&ae) == 3u);
    EXPECT(ce_candidate_count(&ce) >= 1u); /* CE create floor −80 dBFS */

    /*
     * Fresh AE/CE for AS-001: quiet fingerprints must not steal heli identity.
     * Still no activity_detector — proves classification without RMS START.
     */
    ce_stop(&ce);
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_start(&ce, &ccfg));
    EXPECT(ce_bind_audio_engine(&ce, &ae));

    ae_signature_t heli;
    fill_heli(&heli, 1u);
    EXPECT(ae_publish_signature(&ae, &heli));

    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0u, &c));
    EXPECT(c.last_window_seq == 1u);
    EXPECT(c.last_update_mono_us == 1000000);
    ce_classification_t cls;
    EXPECT(ce_get_classification(&ce, c.candidate_id, &cls));
    EXPECT(cls.matched);
    EXPECT(cls.profile_id == AS_PROFILE_ID_AS001);
    EXPECT(c.state != AE_CAND_PROMOTED); /* classification ≠ Observation START */

    ce_stop(&ce);
}

/**
 * B: CE + AS-002 siren as independent AE listeners on one publish.
 * History remains readable; neither consumer empties the ring.
 */
static void test_dual_listeners_shared_ae(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ear_siren_se_public_t siren;
    ae_config_t acfg;
    ce_config_t ccfg;
    ae_config_set_defaults(&acfg);
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 1u;
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_start(&ce, &ccfg));
    EXPECT(ce_bind_audio_engine(&ce, &ae));
    ear_siren_se_public_init(&siren);
    EXPECT(ae_add_signature_listener(&ae, siren_ae_listener, &siren));
    EXPECT(ae_listener_count(&ae) == 2u); /* CE + siren */

    /* Establish siren background EMA (quiet). */
    for (uint32_t seq = 1u; seq <= 3u; seq++) {
        ae_signature_t bg;
        fill_quiet_bg(&bg, seq);
        EXPECT(ae_publish_signature(&ae, &bg));
    }
    EXPECT(ear_siren_se_public_consecutive(&siren) == 0u);

    /* Siren tonal streak: CE may track separately; siren streak independent. */
    for (uint32_t seq = 4u; seq <= 8u; seq++) {
        ae_signature_t sig;
        fill_siren(&sig, seq);
        EXPECT(ae_publish_signature(&ae, &sig));
    }
    EXPECT(ear_siren_se_public_consecutive(&siren) == 5u);
    EXPECT(ear_siren_se_public_allows_promote(&siren));
    EXPECT(ce_candidate_count(&ce) >= 1u);

    /* Shared history still holds published windows (not consumed). */
    ae_signature_t hist[8];
    const uint32_t n = ae_history_copy(&ae, hist, 8);
    EXPECT(n >= 5u);
    EXPECT(hist[0].window_seq == 8u);
    EXPECT(hist[0].window_end_mono_us == 8000000);
    EXPECT(hist[0].quality == AE_QUALITY_OK);

    /* Independent state: clear siren does not wipe CE candidates / AE history. */
    ear_siren_se_public_init(&siren);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 0u);
    EXPECT(ce_candidate_count(&ce) >= 1u);
    EXPECT(ae_history_copy(&ae, hist, 8) >= 5u);

    ce_stop(&ce);
}

/**
 * C+D: GAP/INVALID explicit; temporal continuity; CE interface timing/quality.
 */
static void test_gap_invalid_continuity_and_ce_fields(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ear_siren_se_public_t siren;
    ae_config_t acfg;
    ce_config_t ccfg;
    ae_config_set_defaults(&acfg);
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 1u;
    ccfg.t_coast = 5u;
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_start(&ce, &ccfg));
    EXPECT(ce_bind_audio_engine(&ce, &ae));
    ear_siren_se_public_init(&siren);
    EXPECT(ae_add_signature_listener(&ae, siren_ae_listener, &siren));

    for (uint32_t seq = 1u; seq <= 3u; seq++) {
        ae_signature_t bg;
        fill_quiet_bg(&bg, seq);
        EXPECT(ae_publish_signature(&ae, &bg));
    }
    for (uint32_t seq = 4u; seq <= 8u; seq++) {
        ae_signature_t sig;
        fill_siren(&sig, seq);
        EXPECT(ae_publish_signature(&ae, &sig));
    }
    EXPECT(ear_siren_se_public_consecutive(&siren) == 5u);

    ae_candidate_t before;
    EXPECT(ce_get_candidate(&ce, 0u, &before));
    const uint64_t cid = before.candidate_id;
    const uint32_t gaps_before = before.gap_events;

    /* GAP: explicit quality; siren streak broken; CE coasts (not silent OK). */
    ae_signature_t gap;
    fill_siren(&gap, 9u);
    gap.quality = AE_QUALITY_GAP;
    EXPECT(ae_publish_signature(&ae, &gap));
    EXPECT(ear_siren_se_public_consecutive(&siren) == 0u);
    {
        ear_siren_last_window_t lw;
        ear_siren_se_public_get_last_window(&siren, &lw);
        EXPECT(lw.kind == EAR_SIREN_LAST_UNUSABLE);
    }
    ae_candidate_t mid;
    EXPECT(ce_find_candidate(&ce, cid, &mid));
    EXPECT(mid.gap_events >= gaps_before + 1u);
    EXPECT(mid.state == AE_CAND_COASTING || mid.coast_windows >= 1u);

    ae_signature_t latest;
    EXPECT(ae_get_latest_signature(&ae, &latest));
    EXPECT(latest.window_seq == 9u);
    EXPECT(latest.quality == AE_QUALITY_GAP);

    /* Recovery: next VALID continues; not treated as brand-new silent absence. */
    ae_signature_t ok;
    fill_siren(&ok, 10u);
    EXPECT(ae_publish_signature(&ae, &ok));
    EXPECT(ear_siren_se_public_consecutive(&siren) == 1u);
    {
        ear_siren_last_window_t lw;
        ear_siren_se_public_get_last_window(&siren, &lw);
        EXPECT(lw.kind == EAR_SIREN_LAST_USABLE);
        EXPECT(lw.window_end_mono_us == 10000000);
    }

    /* INVALID: published + tracked; not usable for CE create/update workflow. */
    ae_signature_t inv;
    fill_siren(&inv, 11u);
    inv.quality = AE_QUALITY_INVALID;
    inv.window_end_mono_us = 0; /* also fails time contract if published… */
    /* Publish path rejects missing time — build INVALID with positive time. */
    inv.window_end_mono_us = 11000000;
    EXPECT(ae_publish_signature(&ae, &inv));
    {
        ear_siren_last_window_t lw;
        ear_siren_se_public_get_last_window(&siren, &lw);
        EXPECT(lw.kind == EAR_SIREN_LAST_INVALID);
    }
    EXPECT(!ae_signature_usable_for_tracking(&inv));

    /*
     * After GAP, rematch may update the same id or create a peer track.
     * Continuity requirement: CE still has timed evidence at/after recovery,
     * and the GAP was counted — not a silent “no signature” drop.
     */
    bool have_post_gap = false;
    for (uint8_t i = 0; i < ce_candidate_count(&ce); i++) {
        ae_candidate_t c;
        if (!ce_get_candidate(&ce, i, &c)) {
            continue;
        }
        if (c.last_window_seq >= 10u && c.last_update_mono_us >= 10000000) {
            have_post_gap = true;
            EXPECT(c.quality_summary != AE_QUALITY_INVALID);
            break;
        }
    }
    EXPECT(have_post_gap);
    (void)cid;

    ce_diag_t d;
    ce_get_diag(&ce, &d);
    EXPECT(d.quality_gaps >= 1u);
    EXPECT(d.ignored_unusable >= 1u); /* INVALID at seq 11 */

    ce_stop(&ce);
}

/**
 * Temporal continuity: multiple consecutive OK windows keep shared seq/time
 * alignment across CE fingerprint updates and siren streak growth.
 */
static void test_temporal_multi_window_alignment(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ear_siren_se_public_t siren;
    ae_config_t acfg;
    ce_config_t ccfg;
    ae_config_set_defaults(&acfg);
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 2u;
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_start(&ce, &ccfg));
    EXPECT(ce_bind_audio_engine(&ce, &ae));
    ear_siren_se_public_init(&siren);
    EXPECT(ae_add_signature_listener(&ae, siren_ae_listener, &siren));

    for (uint32_t seq = 1u; seq <= 2u; seq++) {
        ae_signature_t bg;
        fill_quiet_bg(&bg, seq);
        EXPECT(ae_publish_signature(&ae, &bg));
    }

    uint32_t prev_seq = 2u;
    int64_t prev_t = 2000000;
    for (uint32_t seq = 3u; seq <= 7u; seq++) {
        ae_signature_t sig;
        fill_siren(&sig, seq);
        EXPECT(ae_publish_signature(&ae, &sig));
        EXPECT(sig.window_seq == prev_seq + 1u);
        EXPECT(sig.window_end_mono_us > prev_t);
        EXPECT(ae_last_window_seq(&ae) == seq);
        EXPECT(ae_last_window_end_mono_us(&ae) == (int64_t)seq * 1000000LL);
        prev_seq = seq;
        prev_t = sig.window_end_mono_us;
    }
    EXPECT(ear_siren_se_public_consecutive(&siren) == 5u);

    ae_signature_t buf[5];
    const uint32_t n = ae_history_copy_since(&ae, 3000000, buf, 5);
    EXPECT(n == 5u);
    for (uint32_t i = 0; i + 1u < n; i++) {
        EXPECT(buf[i].window_seq > buf[i + 1u].window_seq);
        EXPECT(buf[i].window_end_mono_us > buf[i + 1u].window_end_mono_us);
        EXPECT(buf[i].quality == AE_QUALITY_OK);
    }

    ce_stop(&ce);
}

int main(void)
{
    test_sig_path_without_rms_start();
    test_dual_listeners_shared_ae();
    test_gap_invalid_continuity_and_ce_fields();
    test_temporal_multi_window_alignment();

    if (g_failures != 0) {
        fprintf(
            stderr,
            "test_ae_sig_module_independence: %d failure(s)\n",
            g_failures
        );
        return 1;
    }
    printf("test_ae_sig_module_independence: ok\n");
    return 0;
}
