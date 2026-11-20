/**
 * Host tests: Signature Engine (Architecture A / Rev 3.1).
 *
 * Gate: firmware/scripts/run_firmware_fast_gate.sh (sig_engine)
 */
#define SIG_ENGINE_ALLOW_HOST_HOOKS 1

#include "ae_types.h"
#include "as_profile.h"
#include "ear_siren_se_public.h"
#include "sig_engine.h"

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

static void fill_heli(ae_signature_t *s, uint32_t seq, float rms_dbfs)
{
    fill_base(s, seq, AE_QUALITY_OK);
    s->rms_dbfs = rms_dbfs;
    s->peak_dbfs = rms_dbfs + 7.3f;
    s->crest_db = 7.3f;
    /* BBC / device-bench AS-001 relative band + spectral centers. */
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

static void fill_siren(ae_signature_t *s, uint32_t seq, float rms_dbfs)
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

static void test_classifier_version(void)
{
    EXPECT(strcmp(EAR_CLASSIFIER_VERSION, "1.1") == 0);
    EXPECT(strcmp(sig_engine_classifier_version(), "1.1") == 0);
    char buf[32];
    memset(buf, 'x', sizeof(buf));
    sig_engine_apply_classifier_version(buf, sizeof(buf));
    EXPECT(strcmp(buf, "1.1") == 0);
}

static void test_as001_parity(void)
{
    EXPECT(sig_engine_start());
    ae_signature_t sig;
    as_profile_match_result_t direct;
    as_profile_bg_state_t bg;
    as_profile_bg_init(&bg);
    sig_def_query_t q;
    sig_primary_distance_t prim;

    /* Establish relative background (office-quiet broadband). */
    for (uint32_t seq = 1u; seq <= 4u; seq++) {
        fill_broadband(&sig, seq, -55.0f);
        EXPECT(!as_profile_match_with_bg(as_profile_as001(), &sig, &bg, &direct));
        sig_engine_on_signature(&sig);
    }

    fill_heli(&sig, 5u, -40.0f); /* ~15 dB above bg */
    EXPECT(as_profile_match_with_bg(as_profile_as001(), &sig, &bg, &direct));
    EXPECT(direct.matched);
    sig_engine_on_signature(&sig);
    EXPECT(sig_engine_query(AS_PROFILE_ID_AS001, &q));
    EXPECT(q.matched);
    EXPECT(fabsf(q.score - direct.distance) < 1e-5f);
    EXPECT(sig_engine_get_primary_distance_match(&prim));
    EXPECT(prim.profile_id == AS_PROFILE_ID_AS001);
    EXPECT(prim.window_seq == 5u);

    /* Stateless path must reject uses_rms_rel_bg without bg context. */
    EXPECT(!as_profile_match(as_profile_as001(), &sig, &direct));
    EXPECT(direct.rejected_eligibility == 1u);

    fill_heli(&sig, 6u, -53.0f); /* within margin of bg → no match */
    EXPECT(!as_profile_match_with_bg(as_profile_as001(), &sig, &bg, &direct));
    sig_engine_on_signature(&sig);
    EXPECT(sig_engine_query(AS_PROFILE_ID_AS001, &q));
    EXPECT(!q.matched);
    EXPECT(!sig_engine_get_primary_distance_match(&prim));

    sig_engine_stop();
}

static void test_as002_parity_gap_invalid(void)
{
    EXPECT(sig_engine_start());
    ear_siren_se_public_t ref;
    ear_siren_se_public_init(&ref);

    ae_signature_t sig;
    for (uint32_t seq = 1u; seq <= 3u; seq++) {
        fill_broadband(&sig, seq, -55.0f);
        ear_siren_se_public_on_signature(&ref, &sig);
        sig_engine_on_signature(&sig);
    }
    for (uint32_t seq = 4u; seq <= 8u; seq++) {
        fill_siren(&sig, seq, -31.0f);
        ear_siren_se_public_on_signature(&ref, &sig);
        sig_engine_on_signature(&sig);
    }
    EXPECT(ear_siren_se_public_consecutive(&ref) == 5u);
    EXPECT(sig_engine_allows_promote(AS_PROFILE_ID_AS002));
    {
        sig_def_query_t q;
        EXPECT(sig_engine_query(AS_PROFILE_ID_AS002, &q));
        EXPECT(q.consecutive == 5u);
        EXPECT(q.last_kind == SIG_EV_USABLE);
    }

    /* GAP clears */
    fill_siren(&sig, 9u, -31.0f);
    sig.quality = AE_QUALITY_GAP;
    ear_siren_se_public_on_signature(&ref, &sig);
    sig_engine_on_signature(&sig);
    EXPECT(ear_siren_se_public_consecutive(&ref) == 0u);
    {
        sig_def_query_t q;
        EXPECT(sig_engine_query(AS_PROFILE_ID_AS002, &q));
        EXPECT(q.consecutive == 0u);
        EXPECT(q.last_kind == SIG_EV_GAP);
        EXPECT(q.raw_quality == AE_QUALITY_GAP);
    }

    /* Rebuild then INVALID holds */
    for (uint32_t seq = 10u; seq <= 12u; seq++) {
        fill_siren(&sig, seq, -31.0f);
        ear_siren_se_public_on_signature(&ref, &sig);
        sig_engine_on_signature(&sig);
    }
    EXPECT(ear_siren_se_public_consecutive(&ref) == 3u);
    fill_siren(&sig, 13u, -31.0f);
    sig.quality = AE_QUALITY_INVALID;
    ear_siren_se_public_on_signature(&ref, &sig);
    sig_engine_on_signature(&sig);
    EXPECT(ear_siren_se_public_consecutive(&ref) == 3u);
    {
        sig_def_query_t q;
        EXPECT(sig_engine_query(AS_PROFILE_ID_AS002, &q));
        EXPECT(q.consecutive == 3u);
        EXPECT(q.last_kind == SIG_EV_INVALID);
        EXPECT(!q.gate_vote);
    }

    /* Unexpected quality: not GAP */
    fill_siren(&sig, 14u, -31.0f);
    sig.quality = 99u;
    ear_siren_se_public_on_signature(&ref, &sig);
    sig_engine_on_signature(&sig);
    EXPECT(ear_siren_se_public_consecutive(&ref) == 3u); /* hold */
    {
        sig_def_query_t q;
        EXPECT(sig_engine_query(AS_PROFILE_ID_AS002, &q));
        EXPECT(q.consecutive == 3u);
        EXPECT(q.last_kind == SIG_EV_INVALID);
        EXPECT(q.raw_quality == 99u);
    }

    sig_engine_stop();
}

static void test_multi_hit_and_overflow(void)
{
    EXPECT(sig_engine_host_start_as001_clones(10u));
    EXPECT(sig_engine_enabled_count() == 10u);

    ae_signature_t sig;
    fill_heli(&sig, 42u, -10.0f);
    sig_engine_on_signature(&sig);

    sig_engine_result_t snap;
    EXPECT(sig_engine_copy_result(&snap));
    EXPECT(snap.window_seq == 42u);
    EXPECT(snap.hit_count == SIG_ENGINE_MAX_HITS);
    EXPECT(snap.overflow == 1u);
    EXPECT(snap.truncated == 1u);
    /* Deterministic def_index order → ids 100..107 */
    EXPECT(snap.hits[0].id == 100u);
    EXPECT(snap.hits[7].id == 107u);

    /* Authoritative query for truncated id still matched. */
    sig_def_query_t q;
    EXPECT(sig_engine_query(109u, &q));
    EXPECT(q.matched);
    EXPECT(q.window_seq == 42u);

    /* Primary across all defs (tie → lowest id), not "last in vector". */
    sig_primary_distance_t prim;
    EXPECT(sig_engine_get_primary_distance_match(&prim));
    EXPECT(prim.profile_id == 100u);
    EXPECT(prim.window_seq == 42u);

    sig_engine_stop();
}

static void test_scale_counts(void)
{
    EXPECT(sig_engine_start());
    EXPECT(sig_engine_enabled_count() == 2u); /* Core AS-001 + AS-002 */
    sig_engine_stop();

    EXPECT(sig_engine_host_start_as001_clones(1u));
    EXPECT(sig_engine_enabled_count() == 1u);
    sig_engine_stop();

    EXPECT(sig_engine_host_start_as001_clones(5u));
    EXPECT(sig_engine_enabled_count() == 5u);
    ae_signature_t sig;
    fill_heli(&sig, 1u, -10.0f);
    sig_engine_on_signature(&sig);
    sig_engine_result_t snap;
    EXPECT(sig_engine_copy_result(&snap));
    EXPECT(snap.hit_count == 5u);
    EXPECT(snap.overflow == 0u);
    sig_engine_stop();

    EXPECT(sig_engine_host_start_as001_clones(15u));
    EXPECT(sig_engine_enabled_count() == 15u);
    fill_heli(&sig, 2u, -10.0f);
    sig_engine_on_signature(&sig);
    EXPECT(sig_engine_copy_result(&snap));
    EXPECT(snap.hit_count == SIG_ENGINE_MAX_HITS);
    EXPECT(snap.overflow == 1u);
    {
        sig_def_query_t q;
        EXPECT(sig_engine_query(114u, &q));
        EXPECT(q.matched);
    }
    sig_engine_stop();
}

static void test_copy_consistency(void)
{
    EXPECT(sig_engine_start());
    ae_signature_t sig;
    for (uint32_t seq = 1u; seq <= 4u; seq++) {
        fill_broadband(&sig, seq, -55.0f);
        sig_engine_on_signature(&sig);
    }
    fill_heli(&sig, 7u, -40.0f);
    sig_engine_on_signature(&sig);
    sig_engine_result_t a, b;
    sig_primary_distance_t p;
    sig_def_query_t q;
    EXPECT(sig_engine_copy_result(&a));
    EXPECT(sig_engine_get_primary_distance_match(&p));
    EXPECT(p.valid);
    EXPECT(sig_engine_query(AS_PROFILE_ID_AS001, &q));
    EXPECT(q.matched);
    EXPECT(sig_engine_copy_result(&b));
    EXPECT(a.window_seq == b.window_seq);
    EXPECT(a.window_seq == p.window_seq);
    EXPECT(a.window_seq == q.window_seq);
    EXPECT(a.quality == p.quality);
    sig_engine_stop();
}

/** F3: uniquely best distance match outside the bounded hit vector. */
static void test_primary_best_outside_hit_vector(void)
{
    const uint8_t n = 10u;
    EXPECT(n > SIG_ENGINE_MAX_HITS);
    EXPECT(sig_engine_host_start_as001_distance_ladder(n));

    ae_signature_t sig;
    fill_heli(&sig, 99u, -10.0f);
    sig_engine_on_signature(&sig);

    sig_engine_result_t snap;
    EXPECT(sig_engine_copy_result(&snap));
    EXPECT(snap.overflow == 1u);
    EXPECT(snap.truncated == 1u);
    EXPECT(snap.hit_count == SIG_ENGINE_MAX_HITS);
    /* Vector is def_index order: ids 100..107 — not the best. */
    EXPECT(snap.hits[0].id == 100u);
    EXPECT(snap.hits[SIG_ENGINE_MAX_HITS - 1u].id == 107u);

    const uint16_t best_id = (uint16_t)(100u + n - 1u); /* 109 */
    EXPECT(best_id == 109u);
    /* Best must not appear in the truncated vector. */
    for (uint8_t i = 0u; i < snap.hit_count; i++) {
        EXPECT(snap.hits[i].id != best_id);
    }

    sig_primary_distance_t prim;
    EXPECT(sig_engine_get_primary_distance_match(&prim));
    EXPECT(prim.profile_id == best_id);
    EXPECT(prim.window_seq == 99u);
    EXPECT(prim.valid);

    sig_def_query_t q;
    EXPECT(sig_engine_query(best_id, &q));
    EXPECT(q.matched);
    EXPECT(q.window_seq == 99u);
    /* Uniquely best: better than first vector entry. */
    EXPECT(prim.distance < snap.hits[0].score);

    sig_engine_stop();
}

/** F1: AS-002 consumers use published copy APIs, not a live detector pointer. */
static void test_as002_published_copy_apis(void)
{
    EXPECT(sig_engine_start());
    ae_signature_t sig;
    for (uint32_t seq = 1u; seq <= 3u; seq++) {
        fill_broadband(&sig, seq, -55.0f);
        sig_engine_on_signature(&sig);
    }
    for (uint32_t seq = 4u; seq <= 6u; seq++) {
        fill_siren(&sig, seq, -31.0f);
        sig_engine_on_signature(&sig);
    }
    ear_siren_last_window_t lw;
    EXPECT(sig_engine_copy_as002_last_window(&lw));
    EXPECT(lw.kind == EAR_SIREN_LAST_USABLE);
    EXPECT(lw.tonal_vote);
    EXPECT(sig_engine_allows_promote(AS_PROFILE_ID_AS002));
    EXPECT(sig_engine_as002_consecutive() == 3u);
    ear_cls_evidence_t ev;
    EXPECT(sig_engine_copy_as002_evidence(&ev));
    EXPECT(ev.alarm_ok);

    fill_siren(&sig, 7u, -31.0f);
    sig.quality = AE_QUALITY_GAP;
    sig_engine_on_signature(&sig);
    EXPECT(sig_engine_copy_as002_last_window(&lw));
    EXPECT(lw.kind == EAR_SIREN_LAST_UNUSABLE);
    EXPECT(sig_engine_as002_consecutive() == 0u);
    EXPECT(!sig_engine_copy_as002_evidence(&ev));

    sig_engine_stop();
}

/**
 * Stage A: thin last_window / evidence APIs match full-snapshot fields for
 * the same publication (all production-consumed fields).
 */
static void test_as002_wrapper_matches_snapshot(void)
{
    EXPECT(sig_engine_start());
    ae_signature_t sig;
    for (uint32_t seq = 1u; seq <= 3u; seq++) {
        fill_broadband(&sig, seq, -55.0f);
        sig_engine_on_signature(&sig);
    }
    for (uint32_t seq = 4u; seq <= 6u; seq++) {
        fill_siren(&sig, seq, -31.0f);
        sig_engine_on_signature(&sig);
    }

    sig_as002_snapshot_t snap;
    EXPECT(sig_engine_copy_as002_snapshot(&snap));
    EXPECT(snap.valid);
    EXPECT(snap.evidence_ok != 0u);

    ear_siren_last_window_t lw;
    EXPECT(sig_engine_copy_as002_last_window(&lw));
    EXPECT(lw.kind == snap.last_window.kind);
    EXPECT(lw.tonal_vote == snap.last_window.tonal_vote);
    EXPECT(lw.window_ms == snap.last_window.window_ms);
    EXPECT(lw.window_end_mono_us == snap.last_window.window_end_mono_us);
    EXPECT(lw.has_rms_dbfs == snap.last_window.has_rms_dbfs);
    EXPECT(lw.rms_dbfs == snap.last_window.rms_dbfs);

    ear_cls_evidence_t ev;
    EXPECT(sig_engine_copy_as002_evidence(&ev));
    EXPECT(memcmp(&ev, &snap.evidence, sizeof(ev)) == 0);

    /* GAP: evidence disarmed — wrapper must fail and clear out. */
    fill_siren(&sig, 7u, -31.0f);
    sig.quality = AE_QUALITY_GAP;
    sig_engine_on_signature(&sig);

    memset(&ev, 0xA5, sizeof(ev)); /* poison: must not remain as valid evidence */
    EXPECT(!sig_engine_copy_as002_evidence(&ev));
    {
        ear_cls_evidence_t zeros;
        memset(&zeros, 0, sizeof(zeros));
        EXPECT(memcmp(&ev, &zeros, sizeof(ev)) == 0);
    }

    sig_engine_stop();
}

/**
 * A1/A2 + Stage A: coherent full snapshot and thin wrappers survive N+1.
 *
 * Host note: SIG_PUB_LOCK is a no-op on host. This test proves the
 * coherent-copy API design and deterministic interleaving of publish vs
 * consumer decision — it does NOT prove real ESP32 dual-core lock behavior.
 */
static void test_as002_snapshot_coherent_n_nplus1(void)
{
    EXPECT(sig_engine_start());
    ae_signature_t sig;

    /* Background then three sirens → window N=6, streak=3, promote allowed. */
    for (uint32_t seq = 1u; seq <= 3u; seq++) {
        fill_broadband(&sig, seq, -55.0f);
        sig_engine_on_signature(&sig);
    }
    for (uint32_t seq = 4u; seq <= 6u; seq++) {
        fill_siren(&sig, seq, -31.0f);
        sig_engine_on_signature(&sig);
    }

    /* Publish N: hold full snapshot + thin last_window + evidence. */
    sig_as002_snapshot_t snap_n;
    EXPECT(sig_engine_copy_as002_snapshot(&snap_n));
    EXPECT(snap_n.valid);
    EXPECT(snap_n.window_seq == 6u);
    EXPECT(snap_n.window_end_mono_us == 6000000LL);
    EXPECT(snap_n.quality == (uint8_t)AE_QUALITY_OK);
    EXPECT(snap_n.consecutive == 3u);
    EXPECT(snap_n.allows_promote != 0u);
    EXPECT(snap_n.evidence_ok != 0u);
    EXPECT(snap_n.evidence.alarm_ok);
    EXPECT(snap_n.last_window.kind == EAR_SIREN_LAST_USABLE);
    EXPECT(snap_n.last_window.window_end_mono_us == 6000000LL);

    ear_siren_last_window_t lw_n;
    EXPECT(sig_engine_copy_as002_last_window(&lw_n));
    EXPECT(lw_n.kind == snap_n.last_window.kind);
    EXPECT(lw_n.tonal_vote == snap_n.last_window.tonal_vote);
    EXPECT(lw_n.window_ms == snap_n.last_window.window_ms);
    EXPECT(lw_n.window_end_mono_us == snap_n.last_window.window_end_mono_us);
    EXPECT(lw_n.has_rms_dbfs == snap_n.last_window.has_rms_dbfs);
    EXPECT(lw_n.rms_dbfs == snap_n.last_window.rms_dbfs);

    ear_cls_evidence_t ev_n;
    EXPECT(sig_engine_copy_as002_evidence(&ev_n));
    EXPECT(memcmp(&ev_n, &snap_n.evidence, sizeof(ev_n)) == 0);

    /*
     * Publish N+1 with deliberately different fields (GAP clears streak,
     * forbids promote, drops evidence, marks last_window unusable).
     */
    fill_siren(&sig, 7u, -31.0f);
    sig.quality = AE_QUALITY_GAP;
    sig_engine_on_signature(&sig);

    /* Live / fresh API reads show N+1. */
    EXPECT(!sig_engine_allows_promote(AS_PROFILE_ID_AS002));
    EXPECT(sig_engine_as002_consecutive() == 0u);
    {
        sig_as002_snapshot_t live;
        EXPECT(sig_engine_copy_as002_snapshot(&live));
        EXPECT(live.window_seq == 7u);
        EXPECT(live.quality == (uint8_t)AE_QUALITY_GAP);
        EXPECT(live.consecutive == 0u);
        EXPECT(live.allows_promote == 0u);
        EXPECT(live.evidence_ok == 0u);
        EXPECT(live.last_window.kind == EAR_SIREN_LAST_UNUSABLE);

        ear_siren_last_window_t lw_live;
        EXPECT(sig_engine_copy_as002_last_window(&lw_live));
        EXPECT(lw_live.kind == EAR_SIREN_LAST_UNUSABLE);
        EXPECT(lw_live.window_end_mono_us == live.last_window.window_end_mono_us);

        ear_cls_evidence_t ev_live;
        memset(&ev_live, 0xA5, sizeof(ev_live));
        EXPECT(!sig_engine_copy_as002_evidence(&ev_live));
        {
            ear_cls_evidence_t zeros;
            memset(&zeros, 0, sizeof(zeros));
            EXPECT(memcmp(&ev_live, &zeros, sizeof(ev_live)) == 0);
        }
    }

    /*
     * Held N copies unchanged. Promote decision uses snap_n only
     * (allows_promote + consecutive) — mirrors as002_promote_ok.
     */
    const bool promote_ok =
        (snap_n.valid != 0u) && (snap_n.allows_promote != 0u);
    const float consec_for_policy = (float)snap_n.consecutive;
    EXPECT(promote_ok);
    EXPECT(consec_for_policy == 3.0f);
    EXPECT(snap_n.window_seq == 6u);
    EXPECT(snap_n.window_end_mono_us == 6000000LL);
    EXPECT(snap_n.quality == (uint8_t)AE_QUALITY_OK);
    EXPECT(snap_n.consecutive == 3u);
    EXPECT(snap_n.allows_promote != 0u);
    EXPECT(snap_n.evidence_ok != 0u);
    EXPECT(snap_n.evidence.alarm_ok);
    EXPECT(snap_n.last_window.kind == EAR_SIREN_LAST_USABLE);
    EXPECT(snap_n.last_window.window_end_mono_us == snap_n.window_end_mono_us);

    EXPECT(lw_n.kind == EAR_SIREN_LAST_USABLE);
    EXPECT(lw_n.window_end_mono_us == 6000000LL);
    EXPECT(lw_n.tonal_vote == snap_n.last_window.tonal_vote);
    EXPECT(memcmp(&ev_n, &snap_n.evidence, sizeof(ev_n)) == 0);

    sig_engine_stop();
}

/**
 * Production access audit (source contract documented in-test):
 * - Promotion (`as002_promote_ok`) uses sig_engine_copy_as002_snapshot only.
 * - OM AS-002 window path uses sig_engine_copy_as002_last_window.
 * - CLS evidence path uses sig_engine_copy_as002_evidence.
 * Live ear_siren_se_public_* on as002_live is engine-internal publish only.
 * This host test does not link ear_observation.c; gate + source review confirm.
 */
static void test_as002_production_copy_api_contract(void)
{
    /* Sanity: published copy APIs exist and reject when engine stopped. */
    sig_engine_stop();
    ear_siren_last_window_t lw;
    ear_cls_evidence_t ev;
    sig_as002_snapshot_t snap;
    EXPECT(!sig_engine_copy_as002_snapshot(&snap));
    EXPECT(!sig_engine_copy_as002_last_window(&lw));
    EXPECT(!sig_engine_copy_as002_evidence(&ev));
}

int main(void)
{
    test_classifier_version();
    test_as001_parity();
    test_as002_parity_gap_invalid();
    test_as002_published_copy_apis();
    test_as002_wrapper_matches_snapshot();
    test_as002_snapshot_coherent_n_nplus1();
    test_as002_production_copy_api_contract();
    test_multi_hit_and_overflow();
    test_primary_best_outside_hit_vector();
    test_scale_counts();
    test_copy_consistency();

    if (g_failures != 0) {
        fprintf(stderr, "test_sig_engine: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_sig_engine: ok\n");
    return 0;
}
