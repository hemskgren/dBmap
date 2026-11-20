/**
 * Comparative host tests: AS-001 (as.profile) vs AS-002 (siren detector).
 *
 * Feeds identical ae_signature_t windows into production:
 *   - as_profile_match_with_bg(as_profile_as001(), …)
 *   - ear_siren_se_public_on_signature(…)
 *
 * Does not redesign either algorithm. Documents RMS / lifecycle differences.
 *
 * Gate: firmware/scripts/run_firmware_fast_gate.sh (as001_as002_compare)
 */
#include "ae_types.h"
#include "as_profile.h"
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

/** BBC / device-bench heli spectral+band shape (AS-001 template center). */
static void fill_heli_shape(ae_signature_t *s, uint32_t seq, float rms_dbfs)
{
    fill_base(s, seq, AE_QUALITY_OK);
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

/** Siren-like spectral shape (AS-002 tonal gates). */
static void fill_siren_shape(ae_signature_t *s, uint32_t seq, float rms_dbfs)
{
    fill_base(s, seq, AE_QUALITY_OK);
    s->rms_dbfs = rms_dbfs;
    s->peak_dbfs = rms_dbfs + 7.0f;
    s->crest_db = 7.0f;
    s->band_dbfs[0] = rms_dbfs - 3.0f;
    s->band_dbfs[1] = rms_dbfs;
    s->band_dbfs[2] = rms_dbfs - 6.0f;
    s->band_dbfs[3] = rms_dbfs - 12.0f;
    s->hl_valid = true;
    s->spectral_valid = true;
    s->dominant_hz = 344.0f;
    s->flatness = 0.28f;
    s->centroid_hz = 400.0f;
    s->bandwidth_hz = 80.0f;
}

static void fill_broadband(ae_signature_t *s, uint32_t seq, float rms_dbfs)
{
    fill_base(s, seq, AE_QUALITY_OK);
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

static as_profile_bg_state_t s_as001_bg;

static void as001_reset_bg(void)
{
    as_profile_bg_init(&s_as001_bg);
}

static bool as001_match(const ae_signature_t *sig, as_profile_match_result_t *r)
{
    return as_profile_match_with_bg(as_profile_as001(), sig, &s_as001_bg, r);
}

static void as001_seed_quiet_bg(float rms_dbfs)
{
    as001_reset_bg();
    ae_signature_t sig;
    as_profile_match_result_t r;
    for (uint32_t seq = 1u; seq <= 4u; seq++) {
        fill_broadband(&sig, seq, rms_dbfs);
        (void)as001_match(&sig, &r);
    }
}

static void feed_siren(ear_siren_se_public_t *d, const ae_signature_t *sig)
{
    ear_siren_se_public_on_signature(d, sig);
}

/* ---- A. Amplitude invariance ---- */

static void test_amplitude_invariance(void)
{
    const as_profile_t *p = as_profile_as001();
    EXPECT(p->uses_rms_rel_bg);
    EXPECT(!p->uses_rms);
    EXPECT(p->rms_bg_margin_db == 3.5f);

    as_profile_match_result_t r;
    ae_signature_t sig;

    /* Seed office background, then heli at two absolute levels above margin. */
    as001_seed_quiet_bg(-55.0f);
    fill_heli_shape(&sig, 10u, -40.0f);
    EXPECT(as001_match(&sig, &r));
    EXPECT(r.matched);
    const float dist_loud = r.distance;

    fill_heli_shape(&sig, 11u, -45.0f); /* quieter absolute, still +10 dB vs bg */
    EXPECT(as001_match(&sig, &r));
    EXPECT(r.matched);
    /* Distance is shape-only (no absolute RMS term) → same score. */
    EXPECT(fabsf(r.distance - dist_loud) < 1e-4f);

    /*
     * Same spectral/shape but within bg margin (+2 dB < 3.5): reject.
     * Proves relative baseline gate (not a lowered absolute floor).
     */
    as001_seed_quiet_bg(-55.0f);
    fill_heli_shape(&sig, 12u, -53.0f);
    EXPECT(!as001_match(&sig, &r));
    EXPECT(r.rejected_eligibility == 1u);
    EXPECT(!r.matched);

    /* AS-002: relative to background — absolute level is not the hard gate. */
    ear_siren_se_public_t siren;
    ear_siren_se_public_init(&siren);
    for (uint32_t seq = 1u; seq <= 4u; seq++) {
        fill_broadband(&sig, seq, -55.0f);
        feed_siren(&siren, &sig);
    }
    EXPECT(ear_siren_se_public_consecutive(&siren) == 0u);

    fill_siren_shape(&sig, 5u, -40.0f); /* 15 dB above bg −55 */
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 1u);

    fill_siren_shape(&sig, 6u, -31.0f); /* louder, same spectral */
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 2u);

    /*
     * Same spectral at −54 with bg≈−55: −54 < −55+2.5 (−52.5) → fails start
     * margin. (Fresh detector so streak margin applies; −50 would pass.)
     */
    ear_siren_se_public_init(&siren);
    for (uint32_t seq = 1u; seq <= 4u; seq++) {
        fill_broadband(&sig, seq, -55.0f);
        feed_siren(&siren, &sig);
    }
    fill_siren_shape(&sig, 5u, -54.0f);
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 0u);
}

/* ---- B. Spectral discrimination ---- */

static void test_spectral_discrimination(void)
{
    as_profile_match_result_t r;
    ae_signature_t sig;
    ear_siren_se_public_t siren;

    /* Target-like for each model. */
    as001_seed_quiet_bg(-55.0f);
    fill_heli_shape(&sig, 10u, -40.0f);
    EXPECT(as001_match(&sig, &r));
    EXPECT(r.matched);

    ear_siren_se_public_init(&siren);
    for (uint32_t seq = 1u; seq <= 3u; seq++) {
        fill_broadband(&sig, seq, -55.0f);
        feed_siren(&siren, &sig);
    }
    fill_siren_shape(&sig, 4u, -31.0f);
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 1u);

    /* Same loud RMS as heli, but broadband spectral → AS-001 reject. */
    as001_seed_quiet_bg(-55.0f);
    fill_broadband(&sig, 10u, -40.0f);
    EXPECT(!as001_match(&sig, &r));
    EXPECT(r.rejected_eligibility == 1u); /* flatness/dominant out of gate */

    /* Same RMS as siren, broadband → AS-002 no tonal vote. */
    ear_siren_se_public_init(&siren);
    for (uint32_t seq = 1u; seq <= 3u; seq++) {
        fill_broadband(&sig, seq, -55.0f);
        feed_siren(&siren, &sig);
    }
    fill_broadband(&sig, 4u, -31.0f);
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 0u);

    /*
     * Similar dominant to AS-001 (~230) but flatness too high → reject.
     * Proves spectral shape (not only dominant) matters for AS-001.
     */
    as001_seed_quiet_bg(-55.0f);
    fill_heli_shape(&sig, 20u, -40.0f);
    sig.flatness = 0.60f;
    EXPECT(!as001_match(&sig, &r));
    EXPECT(r.rejected_eligibility == 1u);

    /*
     * Siren-like dominant/flatness: AS-001 fails spectral gates (dom 344
     * outside 100–450 BBC heli band); AS-002 can vote vs bg.
     */
    as001_seed_quiet_bg(-55.0f);
    fill_siren_shape(&sig, 30u, -40.0f);
    EXPECT(!as001_match(&sig, &r));

    ear_siren_se_public_init(&siren);
    for (uint32_t seq = 1u; seq <= 3u; seq++) {
        fill_broadband(&sig, seq, -55.0f);
        feed_siren(&siren, &sig);
    }
    fill_siren_shape(&sig, 4u, -40.0f);
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 1u);

    /*
     * Cross: BBC heli dom ~230 is below AS-002's 300 Hz floor → no siren
     * tonal vote even when level clears bg margin.
     */
    ear_siren_se_public_init(&siren);
    for (uint32_t seq = 1u; seq <= 3u; seq++) {
        fill_broadband(&sig, seq, -55.0f);
        feed_siren(&siren, &sig);
    }
    fill_heli_shape(&sig, 4u, -31.0f);
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 0u);
}

/* ---- C. Background behavior (both use relative baseline; different gates) ---- */

static void test_background_behavior(void)
{
    ae_signature_t sig;
    as_profile_match_result_t r;
    ear_siren_se_public_t siren;

    /* AS-001: without established bg, relative eligibility rejects. */
    as001_reset_bg();
    fill_heli_shape(&sig, 1u, -40.0f);
    EXPECT(!as001_match(&sig, &r));
    EXPECT(r.rejected_eligibility == 1u);

    ear_siren_se_public_init(&siren);

    /* Stable background. */
    for (uint32_t seq = 1u; seq <= 5u; seq++) {
        fill_broadband(&sig, seq, -54.0f);
        feed_siren(&siren, &sig);
    }
    EXPECT(ear_siren_se_public_consecutive(&siren) == 0u);

    /* Gradual bg rise (still non-tonal). */
    for (uint32_t seq = 6u; seq <= 8u; seq++) {
        fill_broadband(&sig, seq, -52.0f + (float)(seq - 6u));
        feed_siren(&siren, &sig);
    }

    /* Target emerges above bg. */
    fill_siren_shape(&sig, 9u, -35.0f);
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 1u);
    fill_siren_shape(&sig, 10u, -34.0f);
    feed_siren(&siren, &sig);
    fill_siren_shape(&sig, 11u, -33.0f);
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) >= 3u);
    EXPECT(ear_siren_se_public_allows_promote(&siren));

    /* Target disappears; unrelated loud broadband RMS remains → streak ends. */
    fill_broadband(&sig, 12u, -20.0f);
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 0u);
    {
        ear_siren_last_window_t lw;
        ear_siren_se_public_get_last_window(&siren, &lw);
        EXPECT(lw.kind == EAR_SIREN_LAST_USABLE);
        EXPECT(!lw.tonal_vote);
    }
}

/* ---- D + F. Temporal / GAP / INVALID / lifecycle semantics ---- */

static void test_temporal_gap_invalid(void)
{
    ae_signature_t sig;
    as_profile_match_result_t r;
    ear_siren_se_public_t siren;

    /* AS-001 is per-window: no streak; GAP/INVALID never match. */
    as001_seed_quiet_bg(-55.0f);
    fill_heli_shape(&sig, 10u, -40.0f);
    EXPECT(as001_match(&sig, &r));
    fill_heli_shape(&sig, 11u, -40.0f);
    sig.quality = AE_QUALITY_GAP;
    EXPECT(!as001_match(&sig, &r));
    EXPECT(r.rejected_eligibility == 1u);
    fill_heli_shape(&sig, 12u, -40.0f);
    sig.quality = AE_QUALITY_INVALID;
    EXPECT(!as001_match(&sig, &r));
    /* Recovery: next OK window matches again independently. */
    fill_heli_shape(&sig, 13u, -40.0f);
    EXPECT(as001_match(&sig, &r));
    EXPECT(r.matched);

    /* AS-002 streak continuity. */
    ear_siren_se_public_init(&siren);
    for (uint32_t seq = 1u; seq <= 3u; seq++) {
        fill_broadband(&sig, seq, -55.0f);
        feed_siren(&siren, &sig);
    }
    for (uint32_t seq = 4u; seq <= 8u; seq++) {
        fill_siren_shape(&sig, seq, -31.0f);
        feed_siren(&siren, &sig);
    }
    EXPECT(ear_siren_se_public_consecutive(&siren) == 5u);

    /* Short absence (one non-tonal) clears streak. */
    fill_broadband(&sig, 9u, -50.0f);
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 0u);

    /* Return after short absence rebuilds streak. */
    for (uint32_t seq = 10u; seq <= 12u; seq++) {
        fill_siren_shape(&sig, seq, -31.0f);
        feed_siren(&siren, &sig);
    }
    EXPECT(ear_siren_se_public_consecutive(&siren) == 3u);

    /* GAP must not count as “signature absent OK evidence” — clears streak. */
    fill_siren_shape(&sig, 13u, -31.0f);
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 4u);
    fill_siren_shape(&sig, 14u, -31.0f);
    sig.quality = AE_QUALITY_GAP;
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 0u);
    {
        ear_siren_last_window_t lw;
        ear_siren_se_public_get_last_window(&siren, &lw);
        EXPECT(lw.kind == EAR_SIREN_LAST_UNUSABLE);
    }

    /* INVALID does not clear streak but is not usable tonal support. */
    for (uint32_t seq = 15u; seq <= 17u; seq++) {
        fill_siren_shape(&sig, seq, -31.0f);
        feed_siren(&siren, &sig);
    }
    EXPECT(ear_siren_se_public_consecutive(&siren) == 3u);
    fill_siren_shape(&sig, 18u, -31.0f);
    sig.quality = AE_QUALITY_INVALID;
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 3u);
    {
        ear_siren_last_window_t lw;
        ear_siren_se_public_get_last_window(&siren, &lw);
        EXPECT(lw.kind == EAR_SIREN_LAST_INVALID);
        EXPECT(!lw.tonal_vote);
    }
    /* Recovery after INVALID continues streak. */
    fill_siren_shape(&sig, 19u, -31.0f);
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 4u);
}

/* ---- E. Below activity ENTER / no RMS START ---- */

static void test_below_activity_enter(void)
{
    /*
     * Typical RMS activity ENTER is −45 dBFS. Signature evaluation must still
     * run on windows (AS-001 may reject on rms_min; AS-002 may vote vs bg).
     */
    ae_signature_t sig;
    as_profile_match_result_t r;
    ear_siren_se_public_t siren;

    as001_seed_quiet_bg(-60.0f);
    fill_heli_shape(&sig, 1u, -58.0f); /* below ENTER and within bg margin */
    EXPECT(!as001_match(&sig, &r));
    EXPECT(r.rejected_eligibility == 1u);

    /* AS-001 at −48 (above ENTER −45, clears bg+3.5 vs −60): matches. */
    as001_seed_quiet_bg(-60.0f);
    fill_heli_shape(&sig, 2u, -48.0f);
    EXPECT(as001_match(&sig, &r));
    EXPECT(r.matched);

    /* AS-002 evaluates quiet-bg then mid-level siren without any START event. */
    ear_siren_se_public_init(&siren);
    for (uint32_t seq = 1u; seq <= 4u; seq++) {
        fill_broadband(&sig, seq, -60.0f);
        feed_siren(&siren, &sig);
    }
    fill_siren_shape(&sig, 5u, -48.0f); /* below ENTER −45, above bg+margin */
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 1u);
}

/* ---- Integration / model differences (documentation asserts) ---- */

static void test_model_contract_differences(void)
{
    const as_profile_t *p = as_profile_as001();
    EXPECT(p->id == AS_PROFILE_ID_AS001);
    EXPECT(!p->uses_rms);
    EXPECT(p->uses_rms_rel_bg);
    EXPECT(p->uses_bands);
    EXPECT(p->uses_spectral);
    EXPECT(!p->uses_pair);

    /* Registry is AS-001 only — AS-002 is not an as.profile entry. */
    EXPECT(as_profile_count() == 1u);
    EXPECT(as_profile_get_by_id(AS_PROFILE_ID_AS001) != NULL);
    EXPECT(as_profile_get_by_id(AS_PROFILE_ID_AS002) == NULL);

    /*
     * Lifecycle semantics (code-backed):
     * - AS-001: per-window distance match; CE owns candidate coast/promote.
     * - AS-002: consecutive tonal streak + last_window kinds; promote via
     *   ear_siren_se_public_allows_promote (≥ ALARM streak), not as.profile.
     */
    EXPECT(EAR_SIREN_ALARM_MIN_STREAK == 3u);
    EXPECT(EAR_SIREN_SE_MIN_STREAK == 5u);
    EXPECT(EAR_SIREN_BG_MARGIN_DB == 2.5f);
}

int main(void)
{
    test_amplitude_invariance();
    test_spectral_discrimination();
    test_background_behavior();
    test_temporal_gap_invalid();
    test_below_activity_enter();
    test_model_contract_differences();

    if (g_failures != 0) {
        fprintf(stderr, "test_as001_as002_compare: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_as001_as002_compare: ok\n");
    return 0;
}
