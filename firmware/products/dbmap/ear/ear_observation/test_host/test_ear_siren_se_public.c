/**
 * Host tests: shared SE public-alert siren evidence + progressive / promote path.
 *
 *   See firmware/scripts/run_firmware_fast_gate.sh (ear_siren_se_public).
 */
#include "ae_engine.h"
#include "as_profile.h"
#include "ce_engine.h"
#include "ear_cls_progressive.h"
#include "ear_obs_manager.h"
#include "ear_promotion_policy.h"
#include "ear_siren_se_public.h"
#include "sp_episode_registry.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(                                                           \
                stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond          \
            );                                                                 \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void fill_sig(
    ae_signature_t *sig,
    uint32_t seq,
    float rms_dbfs,
    float flatness,
    float dominant_hz,
    float crest_db,
    ae_quality_t quality
)
{
    memset(sig, 0, sizeof(*sig));
    sig->schema_version = AE_SIGNATURE_SCHEMA_VERSION;
    sig->window_seq = seq;
    sig->window_end_mono_us = (int64_t)seq * 1000000LL;
    sig->window_ms = 1000u;
    sig->mic_mask = 0x1u;
    sig->quality = (uint8_t)quality;
    sig->rms_dbfs = rms_dbfs;
    sig->peak_dbfs = rms_dbfs + crest_db;
    sig->crest_db = crest_db;
    sig->flatness = flatness;
    sig->dominant_hz = dominant_hz;
    sig->centroid_hz = dominant_hz;
    sig->bandwidth_hz = 80.0f;
    sig->spectral_valid = true;
    sig->band_dbfs[0] = rms_dbfs - 3.0f;
    sig->band_dbfs[1] = rms_dbfs;
    sig->band_dbfs[2] = rms_dbfs - 6.0f;
    sig->band_dbfs[3] = rms_dbfs - 12.0f;
}

static void feed_n(
    ear_siren_se_public_t *det,
    uint32_t *seq,
    unsigned n,
    float rms,
    float flat,
    float dom,
    float crest
)
{
    for (unsigned i = 0; i < n; i++) {
        ae_signature_t sig;
        (*seq)++;
        fill_sig(&sig, *seq, rms, flat, dom, crest, AE_QUALITY_OK);
        ear_siren_se_public_on_signature(det, &sig);
    }
}

static void test_clear_persistent_siren(void)
{
    ear_siren_se_public_t det;
    ear_siren_se_public_init(&det);
    uint32_t seq = 0u;

    /* Quiet background establishes EMA. */
    feed_n(&det, &seq, 4u, -55.0f, 0.70f, 1200.0f, 8.0f);
    EXPECT(ear_siren_se_public_consecutive(&det) == 0u);

    /* Clear mid-tone (0.60-like): flat ~0.29, dom ~344, rms ~−31. */
    feed_n(&det, &seq, 6u, -31.0f, 0.29f, 344.0f, 7.0f);
    EXPECT(ear_siren_se_public_consecutive(&det) == 6u);
    EXPECT(ear_siren_se_public_allows_promote(&det));

    ear_cls_evidence_t ev;
    EXPECT(ear_siren_se_public_fill_evidence(&det, &ev));
    EXPECT(ev.alarm_ok);
    EXPECT(ev.alarm_confidence >= EAR_CLS_ALARM_MIN_CONF);
    EXPECT(ev.se_public_alert_ok);
    EXPECT(ev.se_public_alert_confidence >= EAR_CLS_SE_MIN_CONF);
    EXPECT(!ev.faran_over_ok);
    EXPECT(!ev.vma_ok);
}

static void test_weaker_siren_with_noise(void)
{
    ear_siren_se_public_t det;
    ear_siren_se_public_init(&det);
    uint32_t seq = 0u;

    feed_n(&det, &seq, 5u, -50.0f, 0.65f, 2000.0f, 9.0f);
    /* Quieter 0.50-like tone still tonal vs bg (−50 → −42). */
    feed_n(&det, &seq, 5u, -42.0f, 0.20f, 578.0f, 8.0f);
    EXPECT(ear_siren_se_public_consecutive(&det) == 5u);
    ear_cls_evidence_t ev;
    EXPECT(ear_siren_se_public_fill_evidence(&det, &ev));
    EXPECT(ev.alarm_ok && ev.se_public_alert_ok);
    EXPECT(!ev.faran_over_ok && !ev.vma_ok);
}

static void test_broadband_noise_only(void)
{
    ear_siren_se_public_t det;
    ear_siren_se_public_init(&det);
    uint32_t seq = 0u;
    /* 0.12-like: flatness ≥ 0.55 — no tonal votes. */
    feed_n(&det, &seq, 12u, -48.0f, 0.58f, 400.0f, 10.0f);
    EXPECT(ear_siren_se_public_consecutive(&det) == 0u);
    ear_cls_evidence_t ev;
    EXPECT(!ear_siren_se_public_fill_evidence(&det, &ev));
    EXPECT(!ear_siren_se_public_allows_promote(&det));
}

static void test_short_tonal_insufficient(void)
{
    ear_siren_se_public_t det;
    ear_siren_se_public_init(&det);
    uint32_t seq = 0u;
    feed_n(&det, &seq, 3u, -55.0f, 0.70f, 1500.0f, 8.0f);
    feed_n(&det, &seq, 2u, -30.0f, 0.25f, 344.0f, 7.0f);
    EXPECT(ear_siren_se_public_consecutive(&det) == 2u);
    ear_cls_evidence_t ev;
    EXPECT(!ear_siren_se_public_fill_evidence(&det, &ev));
    EXPECT(!ear_siren_se_public_allows_promote(&det));
}

static void test_frequency_bin_hopping(void)
{
    ear_siren_se_public_t det;
    ear_siren_se_public_init(&det);
    uint32_t seq = 0u;
    feed_n(&det, &seq, 3u, -55.0f, 0.70f, 1800.0f, 8.0f);

    const float hops[] = {344.0f, 578.0f, 688.0f, 344.0f, 688.0f, 578.0f};
    for (unsigned i = 0; i < sizeof(hops) / sizeof(hops[0]); i++) {
        ae_signature_t sig;
        seq++;
        fill_sig(&sig, seq, -32.0f, 0.28f, hops[i], 7.0f, AE_QUALITY_OK);
        ear_siren_se_public_on_signature(&det, &sig);
    }
    EXPECT(ear_siren_se_public_consecutive(&det) == 6u);
    ear_cls_evidence_t ev;
    EXPECT(ear_siren_se_public_fill_evidence(&det, &ev));
    EXPECT(ev.se_public_alert_ok);
}

static void test_interrupted_tonal(void)
{
    ear_siren_se_public_t det;
    ear_siren_se_public_init(&det);
    uint32_t seq = 0u;
    feed_n(&det, &seq, 2u, -55.0f, 0.70f, 1800.0f, 8.0f);
    feed_n(&det, &seq, 4u, -31.0f, 0.27f, 344.0f, 7.0f);
    EXPECT(ear_siren_se_public_consecutive(&det) == 4u);
    /* Noise interruption resets streak. */
    feed_n(&det, &seq, 1u, -50.0f, 0.72f, 2000.0f, 9.0f);
    EXPECT(ear_siren_se_public_consecutive(&det) == 0u);
    feed_n(&det, &seq, 3u, -31.0f, 0.27f, 688.0f, 7.0f);
    EXPECT(ear_siren_se_public_consecutive(&det) == 3u);
    ear_cls_evidence_t ev;
    EXPECT(ear_siren_se_public_fill_evidence(&det, &ev));
    EXPECT(ev.alarm_ok);
    EXPECT(!ev.se_public_alert_ok); /* streak 3 < SE min 5 */
}

static void test_gap_interrupts(void)
{
    ear_siren_se_public_t det;
    ear_siren_se_public_init(&det);
    uint32_t seq = 0u;
    feed_n(&det, &seq, 5u, -31.0f, 0.27f, 344.0f, 7.0f);
    EXPECT(ear_siren_se_public_allows_promote(&det));
    ae_signature_t gap;
    seq++;
    fill_sig(&gap, seq, -60.0f, 1.0f, 0.0f, 0.0f, AE_QUALITY_GAP);
    gap.spectral_valid = false;
    ear_siren_se_public_on_signature(&det, &gap);
    EXPECT(ear_siren_se_public_consecutive(&det) == 0u);
    ear_cls_evidence_t ev;
    EXPECT(!ear_siren_se_public_fill_evidence(&det, &ev));
}

static void test_insufficient_no_classification(void)
{
    ear_siren_se_public_t det;
    ear_siren_se_public_init(&det);
    ear_cls_evidence_t ev;
    EXPECT(!ear_siren_se_public_fill_evidence(&det, &ev));

    ear_cls_state_t st;
    ear_cls_state_init(&st);
    ear_cls_assessment_wire_t wire;
    EXPECT(!ear_cls_apply(&st, &ev, EAR_CLS_PHASE_START, &wire));
    EXPECT(!wire.present);
}

/* ---- Integration: evidence → progressive + promote without AS-001 ---- */

static struct {
    uint32_t starts;
    uint32_t periodics;
    uint32_t ends;
    ear_obs_end_reason_t last_end_reason;
    char start_obs[SP_OBS_EPISODE_ID_LEN];
    char last_obs[SP_OBS_EPISODE_ID_LEN];
    char end_obs[SP_OBS_EPISODE_ID_LEN];
    char last_family[96];
} g_emit;

static uint32_t g_uuid_n;

static void fake_uuid(char out[SP_OBS_EPISODE_ID_LEN])
{
    g_uuid_n++;
    snprintf(
        out,
        SP_OBS_EPISODE_ID_LEN,
        "00000000-0000-4000-8000-%012u",
        (unsigned)g_uuid_n
    );
}

static void path_join(const ear_cls_assessment_wire_t *w, char *out, size_t out_len)
{
    out[0] = '\0';
    if (w == NULL || !w->present || w->n_nodes == 0u || out_len == 0u) {
        return;
    }
    size_t used = 0u;
    for (uint8_t i = 0u; i < w->n_nodes; i++) {
        const char *id = w->nodes[i].id;
        const size_t n = strlen(id);
        if (used + n + (used > 0u ? 1u : 0u) + 1u > out_len) {
            out[0] = '\0';
            return;
        }
        if (used > 0u) {
            out[used++] = '/';
        }
        memcpy(out + used, id, n);
        used += n;
        out[used] = '\0';
    }
}

static bool emit_start(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    const ear_obs_promote_snapshot_t *snap,
    int64_t now_mono_us
)
{
    (void)ctx;
    (void)snap;
    (void)now_mono_us;
    g_emit.starts++;
    memcpy(g_emit.start_obs, binding->observation_id, sizeof(g_emit.start_obs));
    memcpy(g_emit.last_obs, binding->observation_id, sizeof(g_emit.last_obs));
    path_join(&binding->assessment, g_emit.last_family, sizeof(g_emit.last_family));
    return true;
}

static bool emit_periodic(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs
)
{
    (void)ctx;
    (void)interval_start_mono_us;
    (void)interval_end_mono_us;
    (void)has_signal_level_dbfs;
    (void)signal_level_dbfs;
    g_emit.periodics++;
    memcpy(g_emit.last_obs, binding->observation_id, sizeof(g_emit.last_obs));
    path_join(&binding->assessment, g_emit.last_family, sizeof(g_emit.last_family));
    return true;
}

static bool emit_end(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    ear_obs_end_reason_t reason,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs
)
{
    (void)ctx;
    (void)interval_start_mono_us;
    (void)interval_end_mono_us;
    (void)has_signal_level_dbfs;
    (void)signal_level_dbfs;
    g_emit.ends++;
    g_emit.last_end_reason = reason;
    if (binding != NULL) {
        memcpy(g_emit.end_obs, binding->observation_id, sizeof(g_emit.end_obs));
    }
    return true;
}

static void plant_active(ce_engine_t *ce, uint64_t cid, uint32_t evidence, float ema)
{
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (ce->slots[i].in_use) {
            continue;
        }
        ae_candidate_t *c = &ce->slots[i];
        memset(c, 0, sizeof(*c));
        c->in_use = true;
        c->candidate_id = cid;
        c->state = AE_CAND_ACTIVE;
        c->evidence_count = evidence;
        c->ema_rms_dbfs = ema;
        c->quality_summary = AE_QUALITY_OK;
        return;
    }
}

/** Promote one AS002 Observation; returns candidate_id used. */
static uint64_t promote_as002(
    ce_engine_t *ce,
    ear_obs_manager_t *om,
    ear_promotion_policy_t *pol,
    ear_siren_se_public_t *siren,
    uint64_t cid,
    int64_t now_us
)
{
    plant_active(ce, cid, 5u, -30.0f);
    uint32_t seq = 0u;
    feed_n(siren, &seq, 2u, -55.0f, 0.70f, 1800.0f, 8.0f);
    feed_n(siren, &seq, 3u, -31.0f, 0.28f, 344.0f, 7.0f);
    ear_cls_evidence_t ev;
    EXPECT(ear_siren_se_public_fill_evidence(siren, &ev));
    ear_obs_manager_arm_cls_evidence(om, &ev);
    ear_promotion_policy_poll(pol, now_us);
    ear_obs_manager_poll(om, now_us);
    return cid;
}

static void test_integration_siren_promote_and_refine(void)
{
    memset(&g_emit, 0, sizeof(g_emit));
    g_uuid_n = 0u;

    ae_engine_t ae;
    ce_engine_t ce;
    sp_episode_registry_t reg;
    ear_obs_manager_t om;
    ear_promotion_policy_t pol;
    ear_siren_se_public_t siren;

    ce_config_t ccfg;
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 1u;
    EXPECT(ce_start(&ce, &ccfg));

    ae_config_t acfg;
    ae_config_set_defaults(&acfg);
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_bind_audio_engine(&ce, &ae));

    sp_episode_registry_init(&reg);

    ear_obs_manager_config_t mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.registry = &reg;
    mcfg.ce = &ce;
    mcfg.ae = &ae;
    mcfg.uuid_fn = fake_uuid;
    mcfg.observation_interval_ms = 5000u;
    mcfg.emit_start = emit_start;
    mcfg.emit_periodic = emit_periodic;
    mcfg.emit_end = emit_end;
    ear_obs_manager_init(&om, &mcfg);
    ce_set_promotion_cb(&ce, ear_obs_manager_on_promotion, &om);

    ear_promotion_policy_init(&pol, &ce, &om);
    ear_siren_se_public_init(&siren);
    ear_promotion_policy_set_siren(&pol, &siren);

    /* Without siren evidence, ambient ACTIVE must not promote. */
    plant_active(&ce, 77u, 5u, -30.0f);
    ear_promotion_policy_poll(&pol, 1000);
    ear_obs_manager_poll(&om, 1000);
    EXPECT(g_emit.starts == 0u);
    ear_promotion_policy_diag_t diag;
    ear_promotion_policy_get_diag(&pol, &diag);
    EXPECT(diag.last_decision == EAR_PROMO_DECISION_HOLD_NO_CLASS
           || diag.last_decision == EAR_PROMO_DECISION_HOLD_IRRELEVANT);

    /* Build ALARM-only streak (3 windows). */
    uint32_t seq = 0u;
    feed_n(&siren, &seq, 2u, -55.0f, 0.70f, 1800.0f, 8.0f);
    feed_n(&siren, &seq, 3u, -31.0f, 0.28f, 344.0f, 7.0f);
    EXPECT(ear_siren_se_public_allows_promote(&siren));
    EXPECT(ear_siren_se_public_consecutive(&siren) == 3u);

    ear_cls_evidence_t ev;
    EXPECT(ear_siren_se_public_fill_evidence(&siren, &ev));
    EXPECT(ev.alarm_ok && !ev.se_public_alert_ok);
    ear_obs_manager_arm_cls_evidence(&om, &ev);

    ear_promotion_policy_poll(&pol, 2000);
    ear_obs_manager_poll(&om, 2000);
    EXPECT(g_emit.starts == 1u);
    EXPECT(strcmp(g_emit.last_family, "ALARM") == 0);
    ear_promotion_policy_get_diag(&pol, &diag);
    EXPECT(diag.last_profile_id == AS_PROFILE_ID_AS002);
    EXPECT(diag.promote_ok == 1u);
    char obs0[SP_OBS_EPISODE_ID_LEN];
    memcpy(obs0, g_emit.start_obs, sizeof(obs0));
    EXPECT(ear_obs_manager_n_open(&om) == 1u);

    /* Second CE candidate for the same AS-002 signature must not dual-open. */
    plant_active(&ce, 78u, 4u, -32.0f);
    ear_promotion_policy_poll(&pol, 2500);
    ear_obs_manager_poll(&om, 2500);
    EXPECT(g_emit.starts == 1u);
    EXPECT(ear_obs_manager_n_open(&om) == 1u);
    ear_promotion_policy_get_diag(&pol, &diag);
    EXPECT(diag.hold_signature_open >= 1u);
    EXPECT(diag.last_decision == EAR_PROMO_DECISION_HOLD_SIGNATURE_OPEN);

    /* Extend streak to SE and refine same observation_id. */
    feed_n(&siren, &seq, 2u, -31.0f, 0.28f, 688.0f, 7.0f);
    EXPECT(ear_siren_se_public_fill_evidence(&siren, &ev));
    EXPECT(ev.se_public_alert_ok);
    EXPECT(ev.alarm_confidence >= EAR_CLS_ALARM_MIN_CONF);
    EXPECT(ev.se_public_alert_confidence >= EAR_CLS_SE_MIN_CONF);
    /* Independent per-level values (not forced equal). */
    EXPECT(fabsf(ev.alarm_confidence - ev.se_public_alert_confidence) > 0.0f
           || fabsf(ev.alarm_confidence - 0.95f) < 1e-3f);

    EXPECT(ear_obs_manager_feed_cls_evidence(
        &om, 77u, &ev, EAR_CLS_PHASE_PERIODIC
    ));
    ear_obs_manager_on_activity_periodic_hint(&om, 2000, 7000, true, -31.0f);
    EXPECT(g_emit.periodics == 1u);
    EXPECT(strcmp(g_emit.last_obs, obs0) == 0);
    EXPECT(strcmp(g_emit.last_family, "ALARM/SE_PUBLIC_ALERT") == 0);
    EXPECT(g_emit.starts == 1u);
    EXPECT(strstr(g_emit.last_family, "FARAN_OVER") == NULL);
    EXPECT(strstr(g_emit.last_family, "VMA") == NULL);

    ce_stop(&ce);
}

static void mark_as001(ce_engine_t *ce, uint64_t id)
{
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (!ce->slots[i].in_use || ce->slots[i].candidate_id != id) {
            continue;
        }
        ce_classification_t *cls = &ce->classifications[i];
        memset(cls, 0, sizeof(*cls));
        cls->candidate_id = id;
        cls->valid = true;
        cls->matched = true;
        cls->profile_id = AS_PROFILE_ID_AS001;
        cls->acoustic_class = AS_CLASS_HELICOPTER_LIKE;
        cls->distance = 1.0f;
        cls->gap_events_at_match = ce->slots[i].gap_events;
        return;
    }
}

static void test_different_signature_can_coexist(void)
{
    memset(&g_emit, 0, sizeof(g_emit));
    g_uuid_n = 0u;

    ae_engine_t ae;
    ce_engine_t ce;
    sp_episode_registry_t reg;
    ear_obs_manager_t om;
    ear_promotion_policy_t pol;
    ear_siren_se_public_t siren;

    ce_config_t ccfg;
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 1u;
    EXPECT(ce_start(&ce, &ccfg));
    ae_config_t acfg;
    ae_config_set_defaults(&acfg);
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_bind_audio_engine(&ce, &ae));
    sp_episode_registry_init(&reg);

    ear_obs_manager_config_t mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.registry = &reg;
    mcfg.ce = &ce;
    mcfg.ae = &ae;
    mcfg.uuid_fn = fake_uuid;
    mcfg.observation_interval_ms = 5000u;
    mcfg.emit_start = emit_start;
    mcfg.emit_periodic = emit_periodic;
    mcfg.emit_end = emit_end;
    ear_obs_manager_init(&om, &mcfg);
    ce_set_promotion_cb(&ce, ear_obs_manager_on_promotion, &om);
    ear_promotion_policy_init(&pol, &ce, &om);
    ear_siren_se_public_init(&siren);
    ear_promotion_policy_set_siren(&pol, &siren);

    plant_active(&ce, 90u, 5u, -20.0f);
    mark_as001(&ce, 90u);
    ear_promotion_policy_poll(&pol, 1000);
    ear_obs_manager_poll(&om, 1000);
    EXPECT(g_emit.starts == 1u);
    EXPECT(ear_obs_manager_n_open(&om) == 1u);

    plant_active(&ce, 91u, 4u, -30.0f);
    uint32_t seq = 0u;
    feed_n(&siren, &seq, 2u, -55.0f, 0.70f, 1800.0f, 8.0f);
    feed_n(&siren, &seq, 3u, -31.0f, 0.28f, 344.0f, 7.0f);
    ear_cls_evidence_t ev;
    EXPECT(ear_siren_se_public_fill_evidence(&siren, &ev));
    ear_obs_manager_arm_cls_evidence(&om, &ev);
    ear_promotion_policy_poll(&pol, 2000);
    ear_obs_manager_poll(&om, 2000);
    EXPECT(g_emit.starts == 2u);
    EXPECT(ear_obs_manager_n_open(&om) == 2u);
    ear_promotion_policy_diag_t diag;
    ear_promotion_policy_get_diag(&pol, &diag);
    EXPECT(diag.last_profile_id == AS_PROFILE_ID_AS002);
    EXPECT(diag.last_decision == EAR_PROMO_DECISION_PROMOTE);

    ce_stop(&ce);
}

static void test_same_profile_repromote_after_end(void)
{
    memset(&g_emit, 0, sizeof(g_emit));
    g_uuid_n = 0u;

    ae_engine_t ae;
    ce_engine_t ce;
    sp_episode_registry_t reg;
    ear_obs_manager_t om;
    ear_promotion_policy_t pol;
    ear_siren_se_public_t siren;

    ce_config_t ccfg;
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 1u;
    EXPECT(ce_start(&ce, &ccfg));
    ae_config_t acfg;
    ae_config_set_defaults(&acfg);
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_bind_audio_engine(&ce, &ae));
    sp_episode_registry_init(&reg);

    ear_obs_manager_config_t mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.registry = &reg;
    mcfg.ce = &ce;
    mcfg.ae = &ae;
    mcfg.uuid_fn = fake_uuid;
    mcfg.observation_interval_ms = 5000u;
    mcfg.emit_start = emit_start;
    mcfg.emit_periodic = emit_periodic;
    mcfg.emit_end = emit_end;
    ear_obs_manager_init(&om, &mcfg);
    ce_set_promotion_cb(&ce, ear_obs_manager_on_promotion, &om);
    ear_promotion_policy_init(&pol, &ce, &om);
    ear_siren_se_public_init(&siren);
    ear_promotion_policy_set_siren(&pol, &siren);

    plant_active(&ce, 100u, 5u, -30.0f);
    uint32_t seq = 0u;
    feed_n(&siren, &seq, 2u, -55.0f, 0.70f, 1800.0f, 8.0f);
    feed_n(&siren, &seq, 3u, -31.0f, 0.28f, 344.0f, 7.0f);
    ear_cls_evidence_t ev;
    EXPECT(ear_siren_se_public_fill_evidence(&siren, &ev));
    ear_obs_manager_arm_cls_evidence(&om, &ev);
    ear_promotion_policy_poll(&pol, 1000);
    ear_obs_manager_poll(&om, 1000);
    EXPECT(g_emit.starts == 1u);
    EXPECT(ear_obs_manager_has_signature_observation(&om, AS_PROFILE_ID_AS002));

    /* Ambient RMS END must not close AS002; invalid-hold safety still does. */
    ear_obs_manager_on_activity_end_hint(
        &om, EAR_OBS_ACT_END_AMBIENT, 2000, 2100, true, -50.0f
    );
    EXPECT(g_emit.ends == 0u);
    EXPECT(ear_obs_manager_n_open(&om) == 1u);
    ear_obs_manager_on_activity_end_hint(
        &om, EAR_OBS_ACT_END_INVALID_HOLD, 2200, 2300, true, -50.0f
    );
    EXPECT(g_emit.ends == 1u);
    EXPECT(ear_obs_manager_n_open(&om) == 0u);
    EXPECT(!ear_obs_manager_has_signature_observation(&om, AS_PROFILE_ID_AS002));

    /* Prior candidate may still be promotion_entered; use a fresh CE track. */
    EXPECT(ce_reject(&ce, 100u));
    plant_active(&ce, 101u, 5u, -29.0f);
    ear_obs_manager_arm_cls_evidence(&om, &ev);
    ear_promotion_policy_poll(&pol, 3000);
    ear_obs_manager_poll(&om, 3000);
    EXPECT(g_emit.starts == 2u);
    EXPECT(ear_obs_manager_n_open(&om) == 1u);
    ear_promotion_policy_diag_t diag;
    ear_promotion_policy_get_diag(&pol, &diag);
    EXPECT(diag.last_decision == EAR_PROMO_DECISION_PROMOTE);
    EXPECT(diag.last_profile_id == AS_PROFILE_ID_AS002);

    ce_stop(&ce);
}

static void test_pending_same_profile_blocks_policy(void)
{
    memset(&g_emit, 0, sizeof(g_emit));
    g_uuid_n = 0u;

    ae_engine_t ae;
    ce_engine_t ce;
    sp_episode_registry_t reg;
    ear_obs_manager_t om;
    ear_promotion_policy_t pol;

    ce_config_t ccfg;
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 1u;
    EXPECT(ce_start(&ce, &ccfg));
    ae_config_t acfg;
    ae_config_set_defaults(&acfg);
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_bind_audio_engine(&ce, &ae));
    sp_episode_registry_init(&reg);

    ear_obs_manager_config_t mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.registry = &reg;
    mcfg.ce = &ce;
    mcfg.ae = &ae;
    mcfg.uuid_fn = fake_uuid;
    mcfg.observation_interval_ms = 5000u;
    mcfg.emit_start = emit_start;
    mcfg.emit_periodic = emit_periodic;
    mcfg.emit_end = emit_end;
    ear_obs_manager_init(&om, &mcfg);
    ce_set_promotion_cb(&ce, ear_obs_manager_on_promotion, &om);
    ear_promotion_policy_init(&pol, &ce, &om);

    /* Fill both open slots with different profiles via explicit reserve path. */
    plant_active(&ce, 200u, 5u, -20.0f);
    mark_as001(&ce, 200u);
    {
        ear_obs_rsv_handle_t h;
        EXPECT(ear_obs_manager_reserve(&om, &h) == EAR_OBS_RSV_OK);
        EXPECT(ear_obs_manager_bind_reservation_candidate(&om, h, 200u));
        EXPECT(ear_obs_manager_stamp_reservation_relevance(
            &om, h, 200u, AS_PROFILE_ID_AS001, 1.0f
        ));
        EXPECT(ce_promote(&ce, 200u));
        ear_obs_manager_poll(&om, 1000);
    }
    plant_active(&ce, 201u, 5u, -30.0f);
    {
        ear_obs_rsv_handle_t h;
        EXPECT(ear_obs_manager_reserve(&om, &h) == EAR_OBS_RSV_OK);
        EXPECT(ear_obs_manager_bind_reservation_candidate(&om, h, 201u));
        EXPECT(ear_obs_manager_stamp_reservation_relevance(
            &om, h, 201u, AS_PROFILE_ID_AS002, 1.0f
        ));
        EXPECT(ce_promote(&ce, 201u));
        ear_obs_manager_poll(&om, 2000);
    }
    EXPECT(ear_obs_manager_n_open(&om) == 2u);

    /* Force a pending AS-001 while opens are full. */
    plant_active(&ce, 202u, 4u, -25.0f);
    mark_as001(&ce, 202u);
    {
        ear_obs_rsv_handle_t h;
        EXPECT(ear_obs_manager_reserve(&om, &h) == EAR_OBS_RSV_OK);
        EXPECT(ear_obs_manager_bind_reservation_candidate(&om, h, 202u));
        EXPECT(ear_obs_manager_stamp_reservation_relevance(
            &om, h, 202u, AS_PROFILE_ID_AS001, 1.0f
        ));
        EXPECT(ce_promote(&ce, 202u));
        ear_obs_manager_poll(&om, 3000);
    }
    EXPECT(ear_obs_manager_n_pending(&om) == 1u);
    EXPECT(ear_obs_manager_has_signature_observation(&om, AS_PROFILE_ID_AS001));

    /* Policy must not promote another AS-001 while pending holds that profile. */
    plant_active(&ce, 203u, 6u, -22.0f);
    mark_as001(&ce, 203u);
    const uint32_t starts = g_emit.starts;
    ear_promotion_policy_poll(&pol, 4000);
    ear_obs_manager_poll(&om, 4000);
    EXPECT(g_emit.starts == starts);
    ear_promotion_policy_diag_t diag;
    ear_promotion_policy_get_diag(&pol, &diag);
    EXPECT(diag.hold_signature_open >= 1u);

    ce_stop(&ce);
}

static void test_policy_without_siren_unchanged(void)
{
    /* AS-001 path still required when siren not attached. */
    ce_engine_t ce;
    ear_obs_manager_t om;
    ear_promotion_policy_t pol;
    sp_episode_registry_t reg;
    ae_engine_t ae;

    ce_config_t ccfg;
    ce_config_set_defaults(&ccfg);
    EXPECT(ce_start(&ce, &ccfg));
    ae_config_t acfg;
    ae_config_set_defaults(&acfg);
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(ce_bind_audio_engine(&ce, &ae));
    sp_episode_registry_init(&reg);

    ear_obs_manager_config_t mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.registry = &reg;
    mcfg.ce = &ce;
    mcfg.ae = &ae;
    mcfg.uuid_fn = fake_uuid;
    ear_obs_manager_init(&om, &mcfg);
    ear_promotion_policy_init(&pol, &ce, &om);
    /* siren deliberately not set */

    plant_active(&ce, 9u, 4u, -25.0f);
    EXPECT(!ear_promotion_candidate_promote_relevant(&pol, 9u, NULL, NULL));
    {
        uint64_t primary = 0u;
        EXPECT(!ear_promotion_select_primary(&pol, &primary));
    }

    ce_stop(&ce);
}

static void test_last_window_not_stale_consecutive(void)
{
    ear_siren_se_public_t det;
    ear_siren_se_public_init(&det);
    uint32_t seq = 0u;
    feed_n(&det, &seq, 2u, -55.0f, 0.70f, 1800.0f, 8.0f);
    feed_n(&det, &seq, 5u, -31.0f, 0.28f, 344.0f, 7.0f);
    EXPECT(ear_siren_se_public_consecutive(&det) == 5u);

    ae_signature_t inv;
    fill_sig(&inv, ++seq, -31.0f, 0.28f, 344.0f, 7.0f, AE_QUALITY_INVALID);
    ear_siren_se_public_on_signature(&det, &inv);
    /* Streak retained across INVALID, but last_window is not USABLE support. */
    EXPECT(ear_siren_se_public_consecutive(&det) == 5u);
    ear_siren_last_window_t lw;
    ear_siren_se_public_get_last_window(&det, &lw);
    EXPECT(lw.kind == EAR_SIREN_LAST_INVALID);
    EXPECT(!lw.tonal_vote);
}

static void setup_om_siren(
    ae_engine_t *ae,
    ce_engine_t *ce,
    sp_episode_registry_t *reg,
    ear_obs_manager_t *om,
    ear_promotion_policy_t *pol,
    ear_siren_se_public_t *siren,
    uint32_t absence_hold_ms
)
{
    memset(&g_emit, 0, sizeof(g_emit));
    g_uuid_n = 0u;

    ce_config_t ccfg;
    ce_config_set_defaults(&ccfg);
    ccfg.n_enter = 1u;
    EXPECT(ce_start(ce, &ccfg));
    ae_config_t acfg;
    ae_config_set_defaults(&acfg);
    EXPECT(ae_start(ae, &acfg));
    EXPECT(ce_bind_audio_engine(ce, ae));
    sp_episode_registry_init(reg);

    ear_obs_manager_config_t mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.registry = reg;
    mcfg.ce = ce;
    mcfg.ae = ae;
    mcfg.uuid_fn = fake_uuid;
    mcfg.observation_interval_ms = 5000u;
    mcfg.as002_signature_absence_hold_ms = absence_hold_ms;
    mcfg.emit_start = emit_start;
    mcfg.emit_periodic = emit_periodic;
    mcfg.emit_end = emit_end;
    ear_obs_manager_init(om, &mcfg);
    ce_set_promotion_cb(ce, ear_obs_manager_on_promotion, om);
    ear_promotion_policy_init(pol, ce, om);
    ear_siren_se_public_init(siren);
    ear_promotion_policy_set_siren(pol, siren);
}

static void test_as002_signature_absence_end(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    sp_episode_registry_t reg;
    ear_obs_manager_t om;
    ear_promotion_policy_t pol;
    ear_siren_se_public_t siren;
    /* Short hold for host test speed (production default remains 60000). */
    setup_om_siren(&ae, &ce, &reg, &om, &pol, &siren, 5000u);

    (void)promote_as002(&ce, &om, &pol, &siren, 201u, 1000);
    EXPECT(g_emit.starts == 1u);
    EXPECT(ear_obs_manager_n_open(&om) == 1u);
    char obs_as002[SP_OBS_EPISODE_ID_LEN];
    memcpy(obs_as002, g_emit.start_obs, sizeof(obs_as002));

    /* Ambient RMS END must not close AS002 while signature path owns END. */
    ear_obs_manager_on_activity_end_hint(
        &om, EAR_OBS_ACT_END_AMBIENT, 1500, 1600, true, -40.0f
    );
    EXPECT(g_emit.ends == 0u);
    EXPECT(ear_obs_manager_n_open(&om) == 1u);

    /* Positive tonal vote resets absence. */
    ear_obs_manager_on_as002_siren_window(
        &om, true, true, 1000u, 2000000, true, -31.0f
    );
    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&om, &d);
    EXPECT(d.as002_sig_support_reset >= 1u);

    /* Usable non-tonal advances; under hold → still open. */
    for (uint32_t i = 0u; i < 4u; i++) {
        ear_obs_manager_on_as002_siren_window(
            &om,
            true,
            false,
            1000u,
            3000000 + (int64_t)i * 1000000LL,
            true,
            -41.0f
        );
    }
    EXPECT(ear_obs_manager_n_open(&om) == 1u);
    EXPECT(g_emit.ends == 0u);

    /* Fifth usable non-tonal reaches 5000 ms hold → signature END. */
    ear_obs_manager_on_as002_siren_window(
        &om, true, false, 1000u, 8000000, true, -41.0f
    );
    EXPECT(g_emit.ends == 1u);
    EXPECT(g_emit.last_end_reason == EAR_OBS_END_SIGNATURE);
    EXPECT(strcmp(g_emit.end_obs, obs_as002) == 0);
    EXPECT(ear_obs_manager_n_open(&om) == 0u);
    ear_obs_manager_get_diag(&om, &d);
    EXPECT(d.as002_sig_end == 1u);
    EXPECT(d.activity_end_as002_skipped >= 1u);

    ce_stop(&ce);
}

static void test_as002_invalid_pauses_and_invalid_hold_ends(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    sp_episode_registry_t reg;
    ear_obs_manager_t om;
    ear_promotion_policy_t pol;
    ear_siren_se_public_t siren;
    setup_om_siren(&ae, &ce, &reg, &om, &pol, &siren, 3000u);

    (void)promote_as002(&ce, &om, &pol, &siren, 202u, 1000);
    EXPECT(ear_obs_manager_n_open(&om) == 1u);

    /* Advance almost to hold. */
    ear_obs_manager_on_as002_siren_window(
        &om, true, false, 1000u, 2000000, true, -41.0f
    );
    ear_obs_manager_on_as002_siren_window(
        &om, true, false, 1000u, 3000000, true, -41.0f
    );
    EXPECT(ear_obs_manager_n_open(&om) == 1u);

    /* INVALID pauses — must not count as support or complete the hold. */
    ear_obs_manager_on_as002_siren_window(
        &om, false, true /* ignored */, 1000u, 4000000, true, -41.0f
    );
    ear_obs_manager_on_as002_siren_window(
        &om, false, false, 1000u, 5000000, true, -41.0f
    );
    EXPECT(ear_obs_manager_n_open(&om) == 1u);
    EXPECT(g_emit.ends == 0u);
    ear_obs_manager_diag_t d;
    ear_obs_manager_get_diag(&om, &d);
    EXPECT(d.as002_sig_absent_pause >= 2u);

    /* Stale consecutive must not keep Obs: only current usable vote resets. */
    EXPECT(ear_siren_se_public_consecutive(&siren) >= 1u);
    /* After promote feeds, force a non-tonal usable via OM path only. */
    ear_obs_manager_on_as002_siren_window(
        &om, true, false, 1000u, 6000000, true, -41.0f
    );
    EXPECT(g_emit.ends == 1u);
    EXPECT(g_emit.last_end_reason == EAR_OBS_END_SIGNATURE);

    /* Fresh AS002 then INVALID_HOLD activity path ends it. */
    EXPECT(ce_reject(&ce, 202u));
    (void)promote_as002(&ce, &om, &pol, &siren, 203u, 7000);
    EXPECT(ear_obs_manager_n_open(&om) == 1u);
    const uint32_t ends_before = g_emit.ends;
    ear_obs_manager_on_activity_end_hint(
        &om, EAR_OBS_ACT_END_INVALID_HOLD, 8000, 8100, true, -50.0f
    );
    EXPECT(g_emit.ends == ends_before + 1u);
    EXPECT(ear_obs_manager_n_open(&om) == 0u);

    ce_stop(&ce);
}

static void test_as002_sig_end_spares_as001(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    sp_episode_registry_t reg;
    ear_obs_manager_t om;
    ear_promotion_policy_t pol;
    ear_siren_se_public_t siren;
    setup_om_siren(&ae, &ce, &reg, &om, &pol, &siren, 2000u);

    /* AS001 first. */
    plant_active(&ce, 90u, 5u, -20.0f);
    mark_as001(&ce, 90u);
    ear_promotion_policy_poll(&pol, 1000);
    ear_obs_manager_poll(&om, 1000);
    EXPECT(g_emit.starts == 1u);
    char obs_as001[SP_OBS_EPISODE_ID_LEN];
    memcpy(obs_as001, g_emit.start_obs, sizeof(obs_as001));

    plant_active(&ce, 91u, 4u, -30.0f);
    uint32_t seq = 0u;
    feed_n(&siren, &seq, 2u, -55.0f, 0.70f, 1800.0f, 8.0f);
    feed_n(&siren, &seq, 3u, -31.0f, 0.28f, 344.0f, 7.0f);
    ear_cls_evidence_t ev;
    EXPECT(ear_siren_se_public_fill_evidence(&siren, &ev));
    ear_obs_manager_arm_cls_evidence(&om, &ev);
    ear_promotion_policy_poll(&pol, 2000);
    ear_obs_manager_poll(&om, 2000);
    EXPECT(g_emit.starts == 2u);
    EXPECT(ear_obs_manager_n_open(&om) == 2u);
    char obs_as002[SP_OBS_EPISODE_ID_LEN];
    memcpy(obs_as002, g_emit.start_obs, sizeof(obs_as002));

    ear_obs_manager_on_as002_siren_window(
        &om, true, false, 1000u, 3000000, true, -41.0f
    );
    ear_obs_manager_on_as002_siren_window(
        &om, true, false, 1000u, 4000000, true, -41.0f
    );
    EXPECT(g_emit.ends == 1u);
    EXPECT(g_emit.last_end_reason == EAR_OBS_END_SIGNATURE);
    EXPECT(strcmp(g_emit.end_obs, obs_as002) == 0);
    EXPECT(ear_obs_manager_n_open(&om) == 1u);
    EXPECT(ear_obs_manager_has_signature_observation(&om, AS_PROFILE_ID_AS001));
    EXPECT(!ear_obs_manager_has_signature_observation(&om, AS_PROFILE_ID_AS002));

    /* AS001 still ends on ambient activity END. */
    ear_obs_manager_on_activity_end_hint(
        &om, EAR_OBS_ACT_END_AMBIENT, 5000, 5100, true, -50.0f
    );
    EXPECT(g_emit.ends == 2u);
    EXPECT(ear_obs_manager_n_open(&om) == 0u);
    (void)obs_as001;

    ce_stop(&ce);
}

static void test_as002_periodic_unchanged_while_open(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    sp_episode_registry_t reg;
    ear_obs_manager_t om;
    ear_promotion_policy_t pol;
    ear_siren_se_public_t siren;
    setup_om_siren(&ae, &ce, &reg, &om, &pol, &siren, 60000u);

    (void)promote_as002(&ce, &om, &pol, &siren, 210u, 1000);
    EXPECT(ear_obs_manager_n_open(&om) == 1u);

    ear_cls_evidence_t ev;
    EXPECT(ear_siren_se_public_fill_evidence(&siren, &ev));
    EXPECT(ear_obs_manager_feed_cls_evidence(
        &om, 210u, &ev, EAR_CLS_PHASE_PERIODIC
    ));
    ear_obs_manager_on_activity_periodic_hint(&om, 1000, 6000, true, -31.0f);
    EXPECT(g_emit.periodics == 1u);
    EXPECT(strstr(g_emit.last_family, "FARAN_OVER") == NULL);
    EXPECT(strstr(g_emit.last_family, "VMA") == NULL);
    EXPECT(ear_obs_manager_n_open(&om) == 1u);
    EXPECT(g_emit.ends == 0u);

    ce_stop(&ce);
}

int main(void)
{
    test_clear_persistent_siren();
    test_weaker_siren_with_noise();
    test_broadband_noise_only();
    test_short_tonal_insufficient();
    test_frequency_bin_hopping();
    test_interrupted_tonal();
    test_gap_interrupts();
    test_insufficient_no_classification();
    test_last_window_not_stale_consecutive();
    test_integration_siren_promote_and_refine();
    test_different_signature_can_coexist();
    test_same_profile_repromote_after_end();
    test_pending_same_profile_blocks_policy();
    test_policy_without_siren_unchanged();
    test_as002_signature_absence_end();
    test_as002_invalid_pauses_and_invalid_hold_ends();
    test_as002_sig_end_spares_as001();
    test_as002_periodic_unchanged_while_open();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
