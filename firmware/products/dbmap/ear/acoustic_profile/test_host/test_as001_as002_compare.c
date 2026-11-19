/**
 * Comparative host tests: AS-001 (as.profile) vs AS-002 (siren detector).
 *
 * Feeds identical ae_signature_t windows into production:
 *   - as_profile_match(as_profile_as001(), …)
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

/** heli_peak-like spectral+band shape (AS-001 template center). */
static void fill_heli_shape(ae_signature_t *s, uint32_t seq, float rms_dbfs)
{
    fill_base(s, seq, AE_QUALITY_OK);
    s->rms_dbfs = rms_dbfs;
    s->peak_dbfs = rms_dbfs + 7.3f;
    s->crest_db = 7.3f;
    /* Relative bands preserved vs AS-001 band_rel_center. */
    s->band_dbfs[0] = rms_dbfs - 8.21f;
    s->band_dbfs[1] = rms_dbfs - 7.42f;
    s->band_dbfs[2] = rms_dbfs - 16.95f;
    s->band_dbfs[3] = rms_dbfs - 38.84f;
    s->hl_valid = true;
    s->spectral_valid = true;
    s->dominant_hz = 500.0f;
    s->flatness = 0.25f;
    s->centroid_hz = 1060.0f;
    s->bandwidth_hz = 1200.0f;
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

static bool as001_match(const ae_signature_t *sig, as_profile_match_result_t *r)
{
    return as_profile_match(as_profile_as001(), sig, r);
}

static void feed_siren(ear_siren_se_public_t *d, const ae_signature_t *sig)
{
    ear_siren_se_public_on_signature(d, sig);
}

/* ---- A. Amplitude invariance ---- */

static void test_amplitude_invariance(void)
{
    const as_profile_t *p = as_profile_as001();
    EXPECT(p->uses_rms);
    EXPECT(p->rms_min_dbfs == -22.0f);

    as_profile_match_result_t r;
    ae_signature_t sig;

    /* Nominal heli amplitude near template center. */
    fill_heli_shape(&sig, 1u, -10.0f);
    EXPECT(as001_match(&sig, &r));
    EXPECT(r.matched);

    /*
     * Same relative bands + spectral, quieter by 5 dB: still above rms_min,
     * distance grows by AS_W_RMS*5 ≈ 1.0 — should still match.
     */
    fill_heli_shape(&sig, 2u, -15.0f);
    EXPECT(as001_match(&sig, &r));
    EXPECT(r.matched);

    /*
     * Same spectral/shape but below hard rms_min (−22): AS-001 rejects even
     * though spectral gates would pass. Proves absolute RMS eligibility gate.
     */
    fill_heli_shape(&sig, 3u, -25.0f);
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
    fill_heli_shape(&sig, 1u, -10.0f);
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
    fill_broadband(&sig, 10u, -10.0f);
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
     * Similar dominant to AS-001 (~500) but flatness too high → reject.
     * Proves spectral shape (not only dominant) matters for AS-001.
     */
    fill_heli_shape(&sig, 20u, -10.0f);
    sig.flatness = 0.60f;
    EXPECT(!as001_match(&sig, &r));
    EXPECT(r.rejected_eligibility == 1u);

    /*
     * Siren-like dominant/flatness but heli-like absolute RMS: AS-001 fails
     * spectral gates (dom 344 outside 420–620); AS-002 can vote vs bg.
     */
    fill_siren_shape(&sig, 30u, -10.0f);
    EXPECT(!as001_match(&sig, &r));

    ear_siren_se_public_init(&siren);
    for (uint32_t seq = 1u; seq <= 3u; seq++) {
        fill_broadband(&sig, seq, -55.0f);
        feed_siren(&siren, &sig);
    }
    fill_siren_shape(&sig, 4u, -10.0f);
    feed_siren(&siren, &sig);
    EXPECT(ear_siren_se_public_consecutive(&siren) == 1u);

    /* Cross: heli shape is not a siren tonal vote (dom 500 in band, flat ok —
     * actually 500 is in 300–750 and flat 0.25 in 0.12–0.42 — WOULD vote!).
     * Crest/bg still apply. Document real behavior: heli spectral CAN vote
     * siren gates if level clears bg margin — models are not mutually exclusive
     * on every axis. */
    ear_siren_se_public_init(&siren);
    for (uint32_t seq = 1u; seq <= 3u; seq++) {
        fill_broadband(&sig, seq, -55.0f);
        feed_siren(&siren, &sig);
    }
    fill_heli_shape(&sig, 4u, -31.0f);
    feed_siren(&siren, &sig);
    /* heli flatness 0.25 + dom 500 within siren gates → vote true. */
    EXPECT(ear_siren_se_public_consecutive(&siren) == 1u);
}

/* ---- C. Background behavior (AS-002 primary; AS-001 has no bg EMA) ---- */

static void test_background_behavior(void)
{
    ae_signature_t sig;
    as_profile_match_result_t r;
    ear_siren_se_public_t siren;

    /* AS-001: no background model — quiet heli shape fails rms_min only. */
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
    fill_heli_shape(&sig, 1u, -10.0f);
    EXPECT(as001_match(&sig, &r));
    fill_heli_shape(&sig, 2u, -10.0f);
    sig.quality = AE_QUALITY_GAP;
    EXPECT(!as001_match(&sig, &r));
    EXPECT(r.rejected_eligibility == 1u);
    fill_heli_shape(&sig, 3u, -10.0f);
    sig.quality = AE_QUALITY_INVALID;
    EXPECT(!as001_match(&sig, &r));
    /* Recovery: next OK window matches again independently. */
    fill_heli_shape(&sig, 4u, -10.0f);
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

    fill_heli_shape(&sig, 1u, -50.0f); /* below ENTER and below AS-001 rms_min */
    EXPECT(!as001_match(&sig, &r));
    EXPECT(r.rejected_eligibility == 1u);

    /* AS-001 at −20 (above rms_min −22, still above ENTER −45): matches. */
    fill_heli_shape(&sig, 2u, -20.0f);
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
    EXPECT(p->uses_rms);
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
