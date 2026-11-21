/**
 * Host tests: Candidate Manager, Module Runtime, workflow/decision, AE bind.
 *
 * Preferred: included in ./firmware/scripts/run_firmware_fast_gate.sh (--host-only).
 *
 * Standalone from repo root (keep includes aligned with the fast gate):
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/platform/audio/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     -I firmware/platform/mic/include \
 *     -I firmware/platform/node_common/include \
 *     -I firmware/products/dbmap/ear/candidate_engine/include \
 *     -I firmware/products/dbmap/ear/acoustic_profile/include \
 *     -I firmware/products/dbmap/ear/signature_engine/include \
 *     -I firmware/products/dbmap/ear/ear_observation/include \
 *     -I firmware/platform/observation/include \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_history.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_engine.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_time.c \
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
 *     firmware/platform/observation/episode_registry.c \
 *     firmware/products/dbmap/ear/ear_observation/ce_promotion_bridge.c \
 *     firmware/products/dbmap/ear/candidate_engine/test_host/test_ce_architecture.c \
 *     -lm -o /tmp/test_ce_architecture && /tmp/test_ce_architecture
 */
#include "ae_engine.h"
#include "ae_signature.h"
#include "ce_engine.h"
#include "ce_promotion_bridge.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_enter = 0;
static int g_exit = 0;

static void on_promo(const ae_candidate_t *c, ce_promotion_event_t ev, void *ctx)
{
    (void)c;
    (void)ctx;
    if (ev == CE_PROMOTION_ENTER) {
        g_enter++;
    } else if (ev == CE_PROMOTION_EXIT) {
        g_exit++;
    }
}

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

typedef struct {
    const char *name;
    int inits;
    int shutdowns;
    int execs;
    ce_module_status_t force_status;
    int32_t code;
    float score;
    char *order;
    int *order_n;
    const void *seen_cfg;
    bool fail_init;
} spy_mod_t;

static char g_down[8];
static int g_down_n;

static bool spy_init(void *ctx, const void *cfg)
{
    spy_mod_t *s = (spy_mod_t *)ctx;
    if (s->fail_init) {
        return false;
    }
    s->inits++;
    s->seen_cfg = cfg;
    return true;
}

static void spy_shutdown(void *ctx)
{
    spy_mod_t *s = (spy_mod_t *)ctx;
    s->shutdowns++;
    if (s->name != NULL && s->name[0] != '\0' && g_down_n < (int)sizeof(g_down) - 1) {
        g_down[g_down_n++] = s->name[0];
        g_down[g_down_n] = '\0';
    }
}

static ce_module_status_t spy_execute(
    void *ctx,
    const ce_module_input_t *in,
    ce_module_result_t *out
)
{
    spy_mod_t *s = (spy_mod_t *)ctx;
    (void)in;
    s->execs++;
    if (s->order != NULL && s->order_n != NULL && s->name != NULL) {
        const int n = *s->order_n;
        if (n >= 0 && n < 6) {
            s->order[n] = s->name[0];
            s->order[n + 1] = '\0';
            *s->order_n = n + 1;
        }
    }
    if (out != NULL) {
        out->code = s->code;
        out->score = s->score;
    }
    return s->force_status;
}

static void fill_spy_desc(ce_module_desc_t *d, const char *id, const char *cfg)
{
    memset(d, 0, sizeof(*d));
    d->id = id;
    d->version = 3u;
    d->input_requirements = CE_INPUT_SIGNATURE;
    d->init = spy_init;
    d->shutdown = spy_shutdown;
    d->execute = spy_execute;
    d->cfg = cfg;
}

static void fill_l0(dbmap_l0_window_t *l0, float rms, int64_t t_us, bool valid)
{
    memset(l0, 0, sizeof(*l0));
    l0->valid = valid;
    l0->window_end_mono_us = t_us;
    l0->window_ms = 1000;
    l0->sample_count = 16000;
    l0->rms_dbfs = rms;
    l0->peak_dbfs = rms + 6.0f;
    l0->crest_db = 6.0f;
    l0->band_dbfs[0] = rms - 2.0f;
    l0->band_dbfs[1] = rms;
    l0->band_dbfs[2] = rms - 1.0f;
    l0->band_dbfs[3] = rms - 4.0f;
    l0->hl_valid = true;
    l0->hl_db = 1.0f;
}

static bool build_sig(
    ae_signature_t *sig,
    uint32_t seq,
    float rms,
    bool with_l1,
    bool gap,
    bool l0_valid
)
{
    dbmap_l0_window_t l0;
    dbmap_l1_window_t l1;
    fill_l0(&l0, rms, (int64_t)seq * 1000000, l0_valid);
    memset(&l1, 0, sizeof(l1));
    l1.valid = true;
    l1.window_end_mono_us = l0.window_end_mono_us;
    l1.centroid_hz = 1000.0f;
    l1.bandwidth_hz = 200.0f;
    l1.dominant_hz = 1000.0f;
    l1.flatness = 0.2f;
    return ae_signature_build(
        seq,
        &l0,
        with_l1 ? &l1 : NULL,
        NULL,
        NULL,
        gap,
        sig
    );
}

static bool start_ce(ce_engine_t *ce, uint32_t n_enter, uint32_t t_coast)
{
    ce_config_t cfg;
    ce_config_set_defaults(&cfg);
    cfg.n_enter = n_enter;
    cfg.t_coast = t_coast;
    return ce_start(ce, &cfg);
}

static void feed(ce_engine_t *ce, uint32_t seq, float rms, bool with_l1, bool gap)
{
    ae_signature_t sig;
    EXPECT(build_sig(&sig, seq, rms, with_l1, gap, true));
    ce_engine_on_signature(ce, &sig);
}

/**
 * Feed with a shape index so concurrent candidates differ by identity
 * (relative bands / dominant), not by RMS level alone.
 */
static void feed_shaped(
    ce_engine_t *ce,
    uint32_t seq,
    float rms,
    uint32_t shape_id,
    bool gap
)
{
    dbmap_l0_window_t l0;
    dbmap_l1_window_t l1;
    fill_l0(&l0, rms, (int64_t)seq * 1000000, true);
    const float s = (float)shape_id * 4.0f;
    l0.band_dbfs[0] = rms - 2.0f - s;
    l0.band_dbfs[1] = rms - s;
    l0.band_dbfs[2] = rms - 1.0f + s * 0.5f;
    l0.band_dbfs[3] = rms - 4.0f + s;
    memset(&l1, 0, sizeof(l1));
    l1.valid = true;
    l1.window_end_mono_us = l0.window_end_mono_us;
    l1.centroid_hz = 400.0f + (float)shape_id * 350.0f;
    l1.bandwidth_hz = 200.0f;
    l1.dominant_hz = 300.0f + (float)shape_id * 400.0f;
    l1.flatness = 0.15f + (float)shape_id * 0.08f;
    if (l1.flatness > 0.95f) {
        l1.flatness = 0.95f;
    }
    ae_signature_t sig;
    EXPECT(ae_signature_build(seq, &l0, &l1, NULL, NULL, gap, &sig));
    ce_engine_on_signature(ce, &sig);
}

static bool decision_is(const ce_engine_t *ce, ce_decision_t want)
{
    ce_decision_t got;
    if (!ce_engine_last_decision(ce, &got)) {
        return false;
    }
    return got == want;
}

static void test_create_associate_and_ids(void)
{
    ce_engine_t ce;
    EXPECT(start_ce(&ce, 2u, 3u));
    uint16_t ver = 0u;
    EXPECT(ce_module_is_registered(&ce, "ce.evidence", &ver));
    EXPECT(ver == 1u);
    EXPECT(ce_module_is_registered(&ce, "as.profile", &ver));
    EXPECT(ver == 1u);

    feed(&ce, 1u, -40.0f, false, false);
    EXPECT(ce_candidate_count(&ce) == 1u);
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_NASCENT);
    EXPECT(c.candidate_id == 1u);
    EXPECT(decision_is(&ce, CE_DECISION_KEEP));
    const uint64_t id = c.candidate_id;

    feed(&ce, 2u, -40.2f, false, false);
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    EXPECT(c.candidate_id == id);
    EXPECT(c.evidence_count >= 2u);
    EXPECT(c.quality_summary == AE_QUALITY_DEGRADED);
    EXPECT(c.last_evidence_mono_us == 2000000);

    ce_diag_t d;
    ce_get_diag(&ce, &d);
    EXPECT(d.created == 1u);
    EXPECT(d.associated == 1u);
    EXPECT(d.in_use_count == 1u);
    EXPECT(d.promote_successes == 0u);
    EXPECT(d.module_exec >= 2u);
    EXPECT(d.workflow_completed == 2u);
    EXPECT(d.workflow_failed == 0u);
    EXPECT(d.decision_count[CE_DECISION_KEEP] == 2u);
    EXPECT(d.decision_count[CE_DECISION_PROMOTE] == 0u);
    ce_stop(&ce);
}

static void test_two_concurrent(void)
{
    ce_engine_t ce;
    EXPECT(start_ce(&ce, 2u, 3u));
    feed_shaped(&ce, 1u, -40.0f, 0u, false);
    feed_shaped(&ce, 2u, -40.0f, 3u, false);
    EXPECT(ce_candidate_count(&ce) == 2u);
    feed_shaped(&ce, 3u, -40.0f, 0u, false);
    feed_shaped(&ce, 4u, -40.0f, 3u, false);
    EXPECT(ce_candidate_count(&ce) == 2u);
    uint64_t ids[2] = {0u, 0u};
    for (uint8_t i = 0; i < 2u; i++) {
        ae_candidate_t c;
        EXPECT(ce_get_candidate(&ce, i, &c));
        EXPECT(c.state == AE_CAND_ACTIVE);
        ids[i] = c.candidate_id;
    }
    EXPECT(ids[0] != ids[1]);
    EXPECT(ids[0] != 0u);
    ce_stop(&ce);
}

static void test_capacity_evict_and_drop(void)
{
    ce_engine_t ce;
    EXPECT(start_ce(&ce, 2u, 3u));
    for (uint32_t i = 0; i < 9u; i++) {
        feed_shaped(&ce, i + 1u, -40.0f, i, false);
    }
    EXPECT(ce_candidate_count(&ce) == CE_MAX_CANDIDATES);
    ce_diag_t d;
    ce_get_diag(&ce, &d);
    EXPECT(d.created == 9u);
    EXPECT(d.nascent_evicted >= 1u);
    ce_stop(&ce);

    EXPECT(start_ce(&ce, 2u, 3u));
    for (uint32_t i = 0; i < 8u; i++) {
        feed_shaped(&ce, i + 1u, -40.0f, i, false);
    }
    for (uint32_t i = 0; i < 8u; i++) {
        feed_shaped(&ce, 9u + i, -40.0f, i, false);
    }
    EXPECT(ce_candidate_count(&ce) == 8u);
    feed_shaped(&ce, 17u, -40.0f, 10u, false);
    EXPECT(ce_candidate_count(&ce) == 8u);
    ce_get_diag(&ce, &d);
    EXPECT(d.create_dropped_full >= 1u);
    ce_stop(&ce);
}

static void test_expiry_reject_promote_terminal(void)
{
    ce_engine_t ce;
    EXPECT(start_ce(&ce, 2u, 1u));
    feed(&ce, 1u, -40.0f, false, false);
    feed(&ce, 2u, -40.0f, false, false);
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    const uint64_t id = c.candidate_id;

    feed(&ce, 3u, -40.0f, false, true);
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_COASTING);
    EXPECT(c.gap_events >= 1u);
    EXPECT(c.quality_summary == AE_QUALITY_DEGRADED);
    EXPECT(c.last_evidence_mono_us == 2000000);
    EXPECT(decision_is(&ce, CE_DECISION_SKIP));

    feed(&ce, 4u, -40.0f, false, true);
    EXPECT(ce_candidate_count(&ce) == 0u);
    EXPECT(!ce_find_candidate(&ce, id, &c));
    EXPECT(!ce_promote(&ce, id));
    ce_diag_t d;
    ce_get_diag(&ce, &d);
    EXPECT(d.expired >= 1u);
    EXPECT(d.quality_gaps >= 2u);
    ce_stop(&ce);

    EXPECT(start_ce(&ce, 2u, 3u));
    feed(&ce, 1u, -40.0f, false, false);
    feed(&ce, 2u, -40.0f, false, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    const uint64_t rid = c.candidate_id;
    EXPECT(ce_reject(&ce, rid));
    EXPECT(!ce_find_candidate(&ce, rid, &c));
    EXPECT(!ce_promote(&ce, rid));
    EXPECT(!ce_reject(&ce, rid));
    ce_get_diag(&ce, &d);
    EXPECT(d.rejected == 1u);
    EXPECT(d.promote_successes == 0u);

    feed(&ce, 3u, -40.0f, false, false);
    feed(&ce, 4u, -40.0f, false, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.candidate_id != rid);
    EXPECT(!c.promotion_entered);
    EXPECT(c.state == AE_CAND_ACTIVE);
    ce_stop(&ce);

    EXPECT(start_ce(&ce, 2u, 3u));
    g_enter = 0;
    g_exit = 0;
    ce_set_promotion_cb(&ce, on_promo, NULL);
    feed(&ce, 1u, -40.0f, false, false);
    feed(&ce, 2u, -40.0f, false, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(ce_promote(&ce, c.candidate_id));
    EXPECT(ce_find_candidate(&ce, c.candidate_id, &c));
    EXPECT(c.state == AE_CAND_PROMOTED);
    EXPECT(c.promotion_entered);
    EXPECT(!ce_promote(&ce, c.candidate_id));
    ce_get_diag(&ce, &d);
    EXPECT(d.promote_successes == 1u);
    EXPECT(d.promotion_enter_events == 1u);
    EXPECT(g_enter == 1);
    const uint64_t pid = c.candidate_id;
    EXPECT(ce_reject(&ce, pid));
    EXPECT(!ce_find_candidate(&ce, pid, &c));
    EXPECT(!ce_promote(&ce, pid));
    ce_get_diag(&ce, &d);
    EXPECT(d.promotion_exit_events == 1u);
    EXPECT(g_exit == 1);
    EXPECT(d.rejected == 1u);
    ce_stop(&ce);
}

static void test_invalid_gap_and_ae_contract(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ae_config_t acfg;
    ae_config_set_defaults(&acfg);
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(start_ce(&ce, 2u, 3u));
    EXPECT(ce_bind_audio_engine(&ce, &ae));

    ae_signature_t bad;
    EXPECT(build_sig(&bad, 1u, -40.0f, false, false, false));
    EXPECT(bad.quality == AE_QUALITY_INVALID);
    EXPECT(ae_publish_signature(&ae, &bad));
    EXPECT(ce_candidate_count(&ce) == 0u);
    EXPECT(decision_is(&ce, CE_DECISION_ERROR));
    ce_diag_t d;
    ce_get_diag(&ce, &d);
    EXPECT(d.ignored_unusable == 1u);
    EXPECT(d.workflow_failed == 0u);
    EXPECT(d.module_exec == 0u);

    ae_signature_t ok;
    EXPECT(build_sig(&ok, 2u, -40.0f, false, false, true));
    EXPECT(ae_publish_signature(&ae, &ok));
    EXPECT(build_sig(&ok, 3u, -40.0f, false, false, true));
    EXPECT(ae_publish_signature(&ae, &ok));
    EXPECT(ce_candidate_count(&ce) == 1u);
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    const uint32_t evidence = c.evidence_count;

    ae_signature_t rewind = ok;
    rewind.window_seq = 4u;
    rewind.window_end_mono_us = ok.window_end_mono_us;
    EXPECT(!ae_publish_signature(&ae, &rewind));
    EXPECT(ae_publish_rejected_count(&ae) >= 1u);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.evidence_count == evidence);

    ae_signature_t gap;
    EXPECT(build_sig(&gap, 4u, -40.0f, false, true, true));
    EXPECT(gap.quality == AE_QUALITY_GAP);
    EXPECT(ae_publish_signature(&ae, &gap));
    EXPECT(ce_candidate_count(&ce) == 1u);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_COASTING);
    EXPECT(c.gap_events >= 1u);
    EXPECT(c.last_evidence_mono_us == ok.window_end_mono_us);
    EXPECT(decision_is(&ce, CE_DECISION_SKIP));

    ae_signature_t jump;
    EXPECT(build_sig(&jump, 7u, -40.0f, false, false, true));
    EXPECT(ae_publish_signature(&ae, &jump));
    EXPECT(ce_candidate_count(&ce) == 1u);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.gap_events >= 2u);
    EXPECT(c.last_window_seq == 7u);
    EXPECT(c.last_evidence_mono_us == jump.window_end_mono_us);
    ce_get_diag(&ce, &d);
    EXPECT(d.seq_discontinuities >= 1u);
    EXPECT(d.ignored_duplicate == 0u);

    ae_signature_t dup = jump;
    ce_engine_on_signature(&ce, &dup);
    ce_get_diag(&ce, &d);
    EXPECT(d.ignored_duplicate == 1u);
    EXPECT(ce_candidate_count(&ce) == 1u);

    ce_stop(&ce);
    EXPECT(ae_listener_count(&ae) == 0u);
}

static void test_listener_start_stop(void)
{
    ae_engine_t ae;
    ce_engine_t ce;
    ae_config_t acfg;
    ae_config_set_defaults(&acfg);
    EXPECT(ae_start(&ae, &acfg));
    EXPECT(start_ce(&ce, 2u, 3u));
    EXPECT(!ce.listener_bound);
    EXPECT(ce_bind_audio_engine(&ce, &ae));
    EXPECT(ce_bind_audio_engine(&ce, &ae));
    EXPECT(ae_listener_count(&ae) == 1u);

    ae_signature_t sig;
    EXPECT(build_sig(&sig, 1u, -40.0f, false, false, true));
    EXPECT(ae_publish_signature(&ae, &sig));
    EXPECT(ce_candidate_count(&ce) == 1u);
    const uint32_t evidence = ce.slots[0].evidence_count;
    const uint64_t id = ce.slots[0].candidate_id;

    ce_unbind_audio_engine(&ce);
    EXPECT(ae_listener_count(&ae) == 0u);
    EXPECT(ce.started);
    EXPECT(build_sig(&sig, 2u, -40.0f, false, false, true));
    EXPECT(ae_publish_signature(&ae, &sig));
    EXPECT(ce.slots[0].evidence_count == evidence);
    EXPECT(ce.slots[0].candidate_id == id);

    EXPECT(ce_bind_audio_engine(&ce, &ae));
    EXPECT(ae_listener_count(&ae) == 1u);
    ce_stop(&ce);
    EXPECT(!ce.started);
    EXPECT(ae_listener_count(&ae) == 0u);
    EXPECT(!ce.slots[0].in_use);
    EXPECT(ce.diag.stop_cleared == 1u);
    EXPECT(build_sig(&sig, 3u, -40.0f, false, false, true));
    EXPECT(ae_publish_signature(&ae, &sig));
    ce_on_signature(&sig, &ce); /* stopped: must not recreate */
    EXPECT(!ce.slots[0].in_use);
    {
        ae_candidate_t ghost;
        EXPECT(!ce_find_candidate(&ce, id, &ghost));
    }
    (void)evidence;

    EXPECT(start_ce(&ce, 2u, 3u));
    EXPECT(ce_bind_audio_engine(&ce, &ae));
    EXPECT(ae_listener_count(&ae) == 1u);
    EXPECT(build_sig(&sig, 4u, -40.0f, false, false, true));
    EXPECT(ae_publish_signature(&ae, &sig));
    EXPECT(ce_candidate_count(&ce) == 1u);
    ce_stop(&ce);
    EXPECT(ae_listener_count(&ae) == 0u);
    ce_stop(&ce);
    EXPECT(ae_listener_count(&ae) == 0u);
}

static const ce_workflow_step_t k_promote_steps[] = {
    {
        .kind = CE_WK_DECIDE,
        .cond = CE_COND_ALWAYS,
        .decide = CE_DECISION_PROMOTE,
    },
};

static const ce_workflow_t k_promote_wf = {
    .id = "test.promote",
    .steps = k_promote_steps,
    .step_count = 1u,
};

static const ce_workflow_step_t k_reject_steps[] = {
    {
        .kind = CE_WK_DECIDE,
        .cond = CE_COND_ALWAYS,
        .decide = CE_DECISION_REJECT,
    },
};

static const ce_workflow_t k_reject_wf = {
    .id = "test.reject",
    .steps = k_reject_steps,
    .step_count = 1u,
};

static void test_keep_reject_promote_decisions(void)
{
    ce_engine_t ce;
    EXPECT(start_ce(&ce, 2u, 3u));
    feed(&ce, 1u, -40.0f, true, false);
    feed(&ce, 2u, -40.0f, true, false);
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    EXPECT(c.quality_summary == AE_QUALITY_OK);
    EXPECT(decision_is(&ce, CE_DECISION_KEEP));
    EXPECT(!c.promotion_entered);
    ce_stop(&ce);

    EXPECT(start_ce(&ce, 2u, 3u));
    ce_engine_set_workflow(&ce, &k_reject_wf);
    feed(&ce, 1u, -40.0f, false, false);
    EXPECT(ce_candidate_count(&ce) == 0u);
    EXPECT(decision_is(&ce, CE_DECISION_REJECT));
    ce_diag_t d;
    ce_get_diag(&ce, &d);
    EXPECT(d.rejected == 1u);
    EXPECT(d.created == 1u);
    EXPECT(!ce_promote(&ce, 1u));
    ce_stop(&ce);

    EXPECT(start_ce(&ce, 2u, 3u));
    g_enter = 0;
    g_exit = 0;
    ce_set_promotion_cb(&ce, on_promo, NULL);
    ce_engine_set_workflow(&ce, &k_promote_wf);
    feed(&ce, 1u, -40.0f, false, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_NASCENT);
    EXPECT(decision_is(&ce, CE_DECISION_PROMOTE));
    ce_get_diag(&ce, &d);
    EXPECT(d.promote_successes == 0u);

    feed(&ce, 2u, -40.0f, false, false);
    EXPECT(ce_find_candidate(&ce, c.candidate_id, &c));
    EXPECT(c.state == AE_CAND_PROMOTED);
    EXPECT(c.promotion_entered);
    const uint64_t id = c.candidate_id;

    feed(&ce, 3u, -40.0f, false, false);
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_PROMOTED);
    ce_get_diag(&ce, &d);
    EXPECT(d.promote_successes == 1u);
    EXPECT(d.promotion_enter_events == 1u);
    EXPECT(g_enter == 1);
    EXPECT(g_exit == 0);
    ce_stop(&ce);
}

static spy_mod_t g_spy_a;
static spy_mod_t g_spy_b;
static ce_module_desc_t g_desc_a;
static ce_module_desc_t g_desc_b;
static char g_order[8];
static int g_order_n;

static void reset_spies(void)
{
    memset(&g_spy_a, 0, sizeof(g_spy_a));
    memset(&g_spy_b, 0, sizeof(g_spy_b));
    g_spy_a.name = "A";
    g_spy_b.name = "B";
    g_spy_a.force_status = CE_MOD_OK;
    g_spy_b.force_status = CE_MOD_OK;
    g_spy_a.code = 7;
    g_spy_a.score = 0.25f;
    g_order_n = 0;
    g_order[0] = '\0';
    g_spy_a.order = g_order;
    g_spy_b.order = g_order;
    g_spy_a.order_n = &g_order_n;
    g_spy_b.order_n = &g_order_n;
    fill_spy_desc(&g_desc_a, "spy.a", "spy-cfg");
    fill_spy_desc(&g_desc_b, "spy.b", "spy-cfg");
}

static const ce_workflow_step_t k_seq_steps[] = {
    {
        .kind = CE_WK_MODULE,
        .cond = CE_COND_ALWAYS,
        .on_missing = CE_DECISION_ERROR,
        .on_error = CE_DECISION_ERROR,
        .module_id = "spy.a",
    },
    {
        .kind = CE_WK_MODULE,
        .cond = CE_COND_ALWAYS,
        .on_missing = CE_DECISION_ERROR,
        .on_error = CE_DECISION_ERROR,
        .module_id = "spy.b",
    },
    {
        .kind = CE_WK_DECIDE,
        .cond = CE_COND_ALWAYS,
        .decide = CE_DECIDE_FUSE_TRACK,
    },
};

static const ce_workflow_t k_seq_wf = {
    .id = "test.seq",
    .steps = k_seq_steps,
    .step_count = 3u,
};

static const ce_workflow_step_t k_seq_err_steps[] = {
    {
        .kind = CE_WK_MODULE,
        .cond = CE_COND_ALWAYS,
        .on_error = CE_DECISION_ERROR,
        .module_id = "spy.a",
    },
    {
        .kind = CE_WK_MODULE,
        .cond = CE_COND_ALWAYS,
        .on_error = CE_DECISION_ERROR,
        .module_id = "spy.b",
    },
    {
        .kind = CE_WK_DECIDE,
        .cond = CE_COND_ALWAYS,
        .decide = CE_DECISION_PROMOTE,
    },
};

static const ce_workflow_t k_seq_err_wf = {
    .id = "test.seqerr",
    .steps = k_seq_err_steps,
    .step_count = 3u,
};

static const ce_workflow_step_t k_skip_steps[] = {
    {
        .kind = CE_WK_MODULE,
        .cond = CE_COND_QUALITY_OK,
        .on_missing = CE_DECISION_SKIP,
        .on_error = CE_DECISION_ERROR,
        .module_id = "spy.a",
    },
    {
        .kind = CE_WK_DECIDE,
        .cond = CE_COND_ALWAYS,
        .decide = CE_DECISION_KEEP,
    },
};

static const ce_workflow_t k_skip_wf = {
    .id = "test.skip",
    .steps = k_skip_steps,
    .step_count = 2u,
};

static const ce_workflow_step_t k_par_steps[] = {
    {
        .kind = CE_WK_PARALLEL,
        .cond = CE_COND_ALWAYS,
        .on_missing = CE_DECISION_ERROR,
        .on_error = CE_DECISION_ERROR,
        .module_id = "spy.a",
        .module_id_b = "spy.b",
    },
    {
        .kind = CE_WK_DECIDE,
        .cond = CE_COND_ALWAYS,
        .decide = CE_DECIDE_FUSE_TRACK,
    },
};

static const ce_workflow_t k_par_wf = {
    .id = "test.par",
    .steps = k_par_steps,
    .step_count = 2u,
};

static const ce_workflow_step_t k_missing_steps[] = {
    {
        .kind = CE_WK_MODULE,
        .cond = CE_COND_ALWAYS,
        .on_error = CE_DECISION_ERROR,
        .module_id = "no.such.module",
    },
};

static const ce_workflow_t k_missing_wf = {
    .id = "test.missing",
    .steps = k_missing_steps,
    .step_count = 1u,
};

static void test_module_runtime(void)
{
    ce_engine_t ce;
    EXPECT(start_ce(&ce, 2u, 3u));
    reset_spies();
    g_down_n = 0;
    g_down[0] = '\0';
    EXPECT(!ce_module_register(&ce, NULL, &g_spy_a));
    ce_module_desc_t bad = g_desc_a;
    bad.execute = NULL;
    bad.id = "spy.bad";
    EXPECT(!ce_module_register(&ce, &bad, &g_spy_a));
    EXPECT(!ce_module_register(&ce, ce_module_evidence_desc(), NULL));

    g_spy_a.fail_init = true;
    fill_spy_desc(&g_desc_a, "spy.fail", "spy-cfg");
    EXPECT(!ce_module_register(&ce, &g_desc_a, &g_spy_a));
    EXPECT(!ce_module_is_registered(&ce, "spy.fail", NULL));
    g_spy_a.fail_init = false;

    reset_spies();
    EXPECT(ce_module_register(&ce, &g_desc_a, &g_spy_a));
    EXPECT(ce_module_register(&ce, &g_desc_b, &g_spy_b));
    EXPECT(!ce_module_register(&ce, &g_desc_a, &g_spy_a));
    uint16_t ver = 0u;
    EXPECT(ce_module_is_registered(&ce, "spy.a", &ver));
    EXPECT(ver == 3u);
    EXPECT(g_spy_a.inits == 1);
    EXPECT(g_spy_a.seen_cfg != NULL);
    EXPECT(strcmp((const char *)g_spy_a.seen_cfg, "spy-cfg") == 0);

    ce_module_desc_t extra;
    spy_mod_t extra_spy;
    memset(&extra_spy, 0, sizeof(extra_spy));
    extra_spy.name = "C";
    extra_spy.force_status = CE_MOD_OK;
    fill_spy_desc(&extra, "spy.c", "spy-cfg");
    EXPECT(ce_module_register(&ce, &extra, &extra_spy));
    /* Built-ins (2) + spy.a/b/c/d = CE_MAX_MODULES (6); next must fail. */
    ce_module_desc_t slot_d;
    spy_mod_t spy_d;
    memset(&spy_d, 0, sizeof(spy_d));
    spy_d.name = "D";
    fill_spy_desc(&slot_d, "spy.d", "spy-cfg");
    EXPECT(ce_module_register(&ce, &slot_d, &spy_d));
    ce_module_desc_t overflow;
    spy_mod_t overflow_spy;
    memset(&overflow_spy, 0, sizeof(overflow_spy));
    overflow_spy.name = "E";
    fill_spy_desc(&overflow, "spy.e", "spy-cfg");
    EXPECT(!ce_module_register(&ce, &overflow, &overflow_spy));

    g_desc_a.max_exec = 1u;
    ce_module_input_t input;
    ae_signature_t sig;
    EXPECT(build_sig(&sig, 1u, -40.0f, true, false, true));
    ce_module_input_set(&input, &sig, NULL);
    ce_module_result_t result;
    EXPECT(ce_runtime_execute(&ce.runtime, "spy.a", &input, &result) == CE_MOD_OK);
    EXPECT(g_spy_a.execs == 1);
    EXPECT(result.code == 7);
    EXPECT(result.score == 0.25f);
    EXPECT(ce_runtime_execute(&ce.runtime, "spy.a", &input, &result) == CE_MOD_ERROR);
    EXPECT(g_spy_a.execs == 1);
    EXPECT(ce_runtime_execute(&ce.runtime, "missing", &input, &result) == CE_MOD_ERROR);

    ce_module_input_set(&input, NULL, NULL);
    EXPECT(ce_runtime_execute(&ce.runtime, "ce.evidence", &input, &result) == CE_MOD_SKIPPED);
    EXPECT((result.flags & CE_MOD_FLAG_MISSING) != 0u);

    ce_stop(&ce);
    EXPECT(g_spy_b.shutdowns == 1);
    EXPECT(g_spy_a.shutdowns == 1);
    EXPECT(extra_spy.shutdowns == 1);
    EXPECT(spy_d.shutdowns == 1);
    /* Reverse shutdown of spy.d/c/b/a (built-ins also shut down; spies only). */
    EXPECT(strcmp(g_down, "DCBA") == 0);
    const int a_shut = g_spy_a.shutdowns;
    ce_stop(&ce);
    EXPECT(g_spy_a.shutdowns == a_shut);
}

static void test_workflow_paths(void)
{
    ce_engine_t ce;
    EXPECT(start_ce(&ce, 2u, 3u));
    reset_spies();
    g_desc_a.max_exec = 0u;
    EXPECT(ce_module_register(&ce, &g_desc_a, &g_spy_a));
    EXPECT(ce_module_register(&ce, &g_desc_b, &g_spy_b));
    ce_engine_set_workflow(&ce, &k_seq_wf);
    feed(&ce, 1u, -40.0f, true, false);
    EXPECT(strcmp(g_order, "AB") == 0);
    EXPECT(decision_is(&ce, CE_DECISION_KEEP));
    EXPECT(g_spy_a.execs == 1);
    EXPECT(g_spy_b.execs == 1);

    ae_signature_t sig;
    EXPECT(build_sig(&sig, 1u, -40.0f, true, false, true));
    ce_module_input_t input;
    ce_module_input_set(&input, &sig, NULL);
    ce_workflow_result_t wr;
    g_order_n = 0;
    g_order[0] = '\0';
    ce_workflow_run(&ce.runtime, &k_seq_wf, &input, &wr);
    EXPECT(wr.decision == CE_DECISION_KEEP);
    EXPECT(!wr.failed);
    EXPECT(wr.result_count == 2u);
    EXPECT(wr.results[0].score == 0.25f);
    EXPECT(wr.results[0].code == 7);
    EXPECT(strcmp(wr.results[0].module_id, "spy.a") == 0);
    ce_workflow_result_t wr2;
    g_order_n = 0;
    g_order[0] = '\0';
    ce_workflow_run(&ce.runtime, &k_seq_wf, &input, &wr2);
    EXPECT(wr2.decision == wr.decision);
    EXPECT(strcmp(g_order, "AB") == 0);
    ce_stop(&ce);

    EXPECT(start_ce(&ce, 2u, 3u));
    reset_spies();
    g_spy_a.force_status = CE_MOD_ERROR;
    EXPECT(ce_module_register(&ce, &g_desc_a, &g_spy_a));
    EXPECT(ce_module_register(&ce, &g_desc_b, &g_spy_b));
    ce_engine_set_workflow(&ce, &k_seq_err_wf);
    feed(&ce, 1u, -40.0f, true, false);
    EXPECT(g_spy_a.execs == 1);
    EXPECT(g_spy_b.execs == 0);
    EXPECT(decision_is(&ce, CE_DECISION_ERROR));
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_NASCENT);
    EXPECT(!c.promotion_entered);
    ce_diag_t d;
    ce_get_diag(&ce, &d);
    EXPECT(d.workflow_failed == 1u);
    EXPECT(d.promote_successes == 0u);
    EXPECT(d.module_errors >= 1u);
    ce_stop(&ce);

    EXPECT(start_ce(&ce, 2u, 3u));
    reset_spies();
    EXPECT(ce_module_register(&ce, &g_desc_a, &g_spy_a));
    ce_engine_set_workflow(&ce, &k_skip_wf);
    feed(&ce, 1u, -40.0f, false, false);
    EXPECT(g_spy_a.execs == 0);
    EXPECT(decision_is(&ce, CE_DECISION_KEEP));
    feed(&ce, 2u, -40.0f, true, false);
    EXPECT(g_spy_a.execs == 1);
    EXPECT(decision_is(&ce, CE_DECISION_KEEP));

    ce_module_input_set(&input, NULL, NULL);
    ce_workflow_run(&ce.runtime, &k_seq_wf, &input, &wr);
    EXPECT(wr.decision == CE_DECISION_ERROR);
    EXPECT(wr.failed);
    ce_stop(&ce);

    EXPECT(start_ce(&ce, 2u, 3u));
    reset_spies();
    EXPECT(ce_module_register(&ce, &g_desc_a, &g_spy_a));
    EXPECT(ce_module_register(&ce, &g_desc_b, &g_spy_b));
    ce_engine_set_workflow(&ce, &k_par_wf);
    g_order_n = 0;
    g_order[0] = '\0';
    feed(&ce, 1u, -40.0f, true, false);
    EXPECT(strcmp(g_order, "AB") == 0);
    EXPECT(decision_is(&ce, CE_DECISION_KEEP));
    char first[8];
    memcpy(first, g_order, sizeof(first));
    g_order_n = 0;
    g_order[0] = '\0';
    feed(&ce, 2u, -40.0f, true, false);
    EXPECT(strcmp(g_order, first) == 0);
    ce_stop(&ce);

    EXPECT(start_ce(&ce, 2u, 3u));
    ce_engine_set_workflow(&ce, &k_missing_wf);
    feed(&ce, 1u, -40.0f, true, false);
    EXPECT(decision_is(&ce, CE_DECISION_ERROR));
    ce_get_diag(&ce, &d);
    EXPECT(d.module_missing >= 1u);
    EXPECT(d.workflow_failed == 1u);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_NASCENT);
    ce_stop(&ce);
}

static void test_fuse_reads_results_not_score(void)
{
    ae_signature_t sig;
    EXPECT(build_sig(&sig, 1u, -40.0f, true, false, true));
    ce_module_input_t in;
    ce_module_input_set(&in, &sig, NULL);
    ce_module_result_t ok;
    memset(&ok, 0, sizeof(ok));
    ok.status = CE_MOD_OK;
    ok.score = 1.0f;
    EXPECT(ce_decision_fuse_track(&in, &ok, 1u) == CE_DECISION_KEEP);

    ce_module_result_t err = ok;
    err.status = CE_MOD_ERROR;
    EXPECT(ce_decision_fuse_track(&in, &err, 1u) == CE_DECISION_ERROR);

    EXPECT(build_sig(&sig, 2u, -40.0f, false, true, true));
    ce_module_input_set(&in, &sig, NULL);
    EXPECT(in.evidence_gap);
    EXPECT(ce_decision_fuse_track(&in, &ok, 1u) == CE_DECISION_SKIP);

    EXPECT(build_sig(&sig, 3u, -40.0f, false, false, false));
    ce_module_input_set(&in, &sig, NULL);
    EXPECT(in.evidence_invalid);
    EXPECT(ce_decision_fuse_track(&in, &ok, 1u) == CE_DECISION_ERROR);

    ce_module_input_set(&in, NULL, NULL);
    EXPECT(ce_decision_fuse_track(&in, &ok, 1u) == CE_DECISION_ERROR);

    EXPECT(build_sig(&sig, 4u, -40.0f, false, false, true));
    EXPECT(sig.quality == AE_QUALITY_DEGRADED);
    ce_module_input_set(&in, &sig, NULL);
    EXPECT(in.evidence_degraded);
    ok.flags = CE_MOD_FLAG_DEGRADED;
    EXPECT(ce_decision_fuse_track(&in, &ok, 1u) == CE_DECISION_KEEP);
}

static void test_deterministic_pair(void)
{
    ce_engine_t a;
    ce_engine_t b;
    EXPECT(start_ce(&a, 2u, 3u));
    EXPECT(start_ce(&b, 2u, 3u));
    for (uint32_t seq = 1u; seq <= 3u; seq++) {
        ae_signature_t sig;
        EXPECT(build_sig(&sig, seq, -40.0f + (float)seq * 0.1f, false, false, true));
        ce_engine_on_signature(&a, &sig);
        ce_engine_on_signature(&b, &sig);
    }
    EXPECT(ce_candidate_count(&a) == ce_candidate_count(&b));
    ae_candidate_t ca;
    ae_candidate_t cb;
    EXPECT(ce_get_candidate(&a, 0, &ca));
    EXPECT(ce_get_candidate(&b, 0, &cb));
    EXPECT(ca.candidate_id == cb.candidate_id);
    EXPECT(ca.state == cb.state);
    EXPECT(ca.evidence_count == cb.evidence_count);
    EXPECT(ca.last_window_seq == cb.last_window_seq);
    EXPECT(ca.last_evidence_mono_us == cb.last_evidence_mono_us);
    EXPECT(ca.confidence == cb.confidence);
    ce_decision_t da;
    ce_decision_t db;
    EXPECT(ce_engine_last_decision(&a, &da));
    EXPECT(ce_engine_last_decision(&b, &db));
    EXPECT(da == db);
    EXPECT(da == CE_DECISION_KEEP);
    ce_stop(&a);
    ce_stop(&b);
}

static void test_bridge_explicit_promotion(void)
{
    ce_engine_t ce;
    ce_promotion_bridge_t bridge;
    EXPECT(start_ce(&ce, 2u, 3u));
    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);

    feed(&ce, 1u, -40.0f, false, false);
    feed(&ce, 2u, -40.0f, false, false);
    ae_candidate_t c;
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    EXPECT(!c.promotion_entered);
    ce_bridge_diag_t bd;
    ce_bridge_get_diag(&bridge, &bd);
    EXPECT(bd.enter_queued == 0u);
    EXPECT(!ce_bridge_is_bound(&bridge));

    EXPECT(ce_promote(&ce, c.candidate_id));
    ce_bridge_get_diag(&bridge, &bd);
    EXPECT(bd.enter_queued == 1u);
    EXPECT(!ce_promote(&ce, c.candidate_id));
    ce_bridge_get_diag(&bridge, &bd);
    EXPECT(bd.enter_queued == 1u);

    sp_ep_handle_t handle;
    memset(&handle, 0, sizeof(handle));
    ce_bridge_notify_obs_open(&bridge, "obs-arch-1", handle);
    ce_bridge_poll(&bridge, NULL);
    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == c.candidate_id);
    EXPECT(strcmp(ce_bridge_bound_observation_id(&bridge), "obs-arch-1") == 0);
    ce_bridge_get_diag(&bridge, &bd);
    const uint32_t assoc = bd.assoc_success;
    EXPECT(assoc == 1u);
    ce_bridge_poll(&bridge, NULL);
    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == c.candidate_id);
    ce_bridge_get_diag(&bridge, &bd);
    EXPECT(bd.assoc_success == assoc);
    EXPECT(bd.enter_queued == 1u);

    EXPECT(ce_reject(&ce, c.candidate_id));
    ce_bridge_poll(&bridge, NULL);
    EXPECT(!ce_bridge_is_bound(&bridge));
    EXPECT(!ce_promote(&ce, c.candidate_id));
    ce_stop(&ce);

    EXPECT(start_ce(&ce, 2u, 3u));
    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);
    ce_engine_set_workflow(&ce, &k_promote_wf);
    feed(&ce, 1u, -42.0f, false, false);
    feed(&ce, 2u, -42.0f, false, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_PROMOTED);
    ce_bridge_get_diag(&bridge, &bd);
    EXPECT(bd.enter_queued == 1u);
    feed(&ce, 3u, -42.0f, false, false);
    ce_bridge_get_diag(&bridge, &bd);
    EXPECT(bd.enter_queued == 1u);
    ce_bridge_notify_obs_open(&bridge, "obs-arch-2", handle);
    ce_bridge_poll(&bridge, NULL);
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == c.candidate_id);
    EXPECT(strcmp(ce_bridge_bound_observation_id(&bridge), "obs-arch-2") == 0);
    ce_bridge_poll(&bridge, NULL);
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == c.candidate_id);
    ce_bridge_get_diag(&bridge, &bd);
    EXPECT(bd.assoc_success == 1u);
    ce_stop(&ce);
}

/**
 * Pre-OTA: ce_stop with ACTIVE / PROMOTED / REJECTED / EXPIRED.
 * PROMOTED must emit EXIT so Bridge association cannot outlive CE.
 * Observation notify_obs_open stays open — CE does not close it.
 */
static void test_stop_with_lifecycle_states(void)
{
    ce_engine_t ce;
    ce_promotion_bridge_t bridge;
    sp_ep_handle_t handle;
    memset(&handle, 0, sizeof(handle));
    ae_candidate_t c;
    ce_diag_t d;
    ce_bridge_diag_t bd;

    /* ACTIVE: no EXIT, slots cleared, no Bridge association. */
    EXPECT(start_ce(&ce, 2u, 3u));
    g_enter = 0;
    g_exit = 0;
    ce_set_promotion_cb(&ce, on_promo, NULL);
    feed(&ce, 1u, -40.0f, false, false);
    feed(&ce, 2u, -40.0f, false, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    const uint64_t active_id = c.candidate_id;
    ce_stop(&ce);
    EXPECT(g_enter == 0);
    EXPECT(g_exit == 0);
    EXPECT(!ce.started);
    EXPECT(ce_candidate_count(&ce) == 0u);
    EXPECT(!ce_find_candidate(&ce, active_id, &c));
    EXPECT(!ce_promote(&ce, active_id));
    EXPECT(ce.diag.stop_cleared == 1u);
    EXPECT(ce.diag.expired == 0u);
    EXPECT(ce.diag.promotion_exit_events == 0u);
    ce_stop(&ce); /* idempotent */

    /* PROMOTED: one EXIT; Bridge association clears on poll; Observation
     * context remains open (CE does not call notify_obs_closed). */
    EXPECT(start_ce(&ce, 2u, 3u));
    g_enter = 0;
    g_exit = 0;
    ce_bridge_init(&bridge);
    ce_bridge_bind_engine(&bridge, &ce);
    ce_set_promotion_cb(&ce, ce_bridge_on_promotion, &bridge);
    feed(&ce, 1u, -40.0f, false, false);
    feed(&ce, 2u, -40.0f, false, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(ce_promote(&ce, c.candidate_id));
    const uint64_t promo_id = c.candidate_id;
    ce_bridge_notify_obs_open(&bridge, "obs-stop-1", handle);
    ce_bridge_poll(&bridge, NULL);
    EXPECT(ce_bridge_is_bound(&bridge));
    EXPECT(ce_bridge_bound_candidate_id(&bridge) == promo_id);
    EXPECT(bridge.obs_context_valid);
    ce_bridge_get_diag(&bridge, &bd);
    EXPECT(bd.enter_queued == 1u);
    EXPECT(bd.exit_queued == 0u);

    ce_stop(&ce);
    EXPECT(!ce.started);
    EXPECT(ce_candidate_count(&ce) == 0u);
    EXPECT(!ce_promote(&ce, promo_id));
    EXPECT(ce.diag.stop_cleared == 1u);
    EXPECT(ce.diag.expired == 0u);
    EXPECT(ce.diag.promotion_exit_events == 1u);
    ce_bridge_get_diag(&bridge, &bd);
    EXPECT(bd.exit_queued == 1u);
    /* Association still bound until drain — stop queued EXIT only. */
    EXPECT(ce_bridge_is_bound(&bridge));
    ce_bridge_poll(&bridge, NULL);
    EXPECT(!ce_bridge_is_bound(&bridge));
    ce_bridge_get_diag(&bridge, &bd);
    EXPECT(bd.disassoc_on_exit == 1u);
    /* Observation context must remain; CE stop is not obs close. */
    EXPECT(bridge.obs_context_valid);
    EXPECT(strcmp(bridge.obs_id, "obs-stop-1") == 0);

    /* REJECTED: already not in_use — stop clears nothing further. */
    EXPECT(start_ce(&ce, 2u, 3u));
    g_enter = 0;
    g_exit = 0;
    ce_set_promotion_cb(&ce, on_promo, NULL);
    feed(&ce, 1u, -40.0f, false, false);
    feed(&ce, 2u, -40.0f, false, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    const uint64_t rej_id = c.candidate_id;
    EXPECT(ce_reject(&ce, rej_id));
    EXPECT(g_exit == 0);
    EXPECT(ce_candidate_count(&ce) == 0u);
    ce_get_diag(&ce, &d);
    EXPECT(d.rejected == 1u);
    EXPECT(d.stop_cleared == 0u);
    ce_stop(&ce);
    EXPECT(g_exit == 0);
    EXPECT(ce.diag.stop_cleared == 0u);
    EXPECT(ce.diag.promotion_exit_events == 0u);
    EXPECT(!ce_promote(&ce, rej_id));

    /* EXPIRED via coast: no EXIT when never promoted; stop is no-op on slots. */
    EXPECT(start_ce(&ce, 2u, 1u));
    g_enter = 0;
    g_exit = 0;
    ce_set_promotion_cb(&ce, on_promo, NULL);
    feed(&ce, 1u, -40.0f, false, false);
    feed(&ce, 2u, -40.0f, false, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    const uint64_t exp_id = c.candidate_id;
    feed(&ce, 3u, -40.0f, false, true);
    feed(&ce, 4u, -40.0f, false, true);
    EXPECT(ce_candidate_count(&ce) == 0u);
    EXPECT(g_exit == 0);
    ce_get_diag(&ce, &d);
    EXPECT(d.expired >= 1u);
    const uint32_t expired_before = d.expired;
    ce_stop(&ce);
    EXPECT(g_exit == 0);
    EXPECT(ce.diag.stop_cleared == 0u);
    EXPECT(ce.diag.expired == expired_before);
    EXPECT(!ce_promote(&ce, exp_id));

    /* PROMOTED then stop without Bridge poll still clears CE slots. */
    EXPECT(start_ce(&ce, 2u, 3u));
    g_enter = 0;
    g_exit = 0;
    ce_set_promotion_cb(&ce, on_promo, NULL);
    feed(&ce, 1u, -40.0f, false, false);
    feed(&ce, 2u, -40.0f, false, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(ce_promote(&ce, c.candidate_id));
    EXPECT(g_enter == 1);
    ce_stop(&ce);
    EXPECT(g_exit == 1);
    EXPECT(ce.diag.stop_cleared == 1u);
    EXPECT(ce.diag.promotion_exit_events == 1u);
    EXPECT(!ce_find_candidate(&ce, c.candidate_id, &c));
}

/**
 * GAP must not run classification modules; INVALID must not mutate lifecycle;
 * repeated GAP → coast → expire; rematch recovers; seq discontinuity coasts.
 */
static void test_gap_invalid_semantics(void)
{
    ce_engine_t ce;
    ae_candidate_t c;
    ce_diag_t d;
    reset_spies();
    g_desc_a.max_exec = 0u;

    EXPECT(start_ce(&ce, 2u, 2u));
    EXPECT(ce_module_register(&ce, &g_desc_a, &g_spy_a));
    const ce_workflow_step_t gap_mod_steps[] = {
        {
            .kind = CE_WK_MODULE,
            .cond = CE_COND_ALWAYS,
            .on_missing = CE_DECISION_ERROR,
            .on_error = CE_DECISION_ERROR,
            .module_id = "spy.a",
        },
        {
            .kind = CE_WK_DECIDE,
            .cond = CE_COND_ALWAYS,
            .decide = CE_DECIDE_FUSE_TRACK,
        },
    };
    const ce_workflow_t gap_mod_wf = {
        .id = "test.gap_mod",
        .steps = gap_mod_steps,
        .step_count = 2u,
    };
    ce_engine_set_workflow(&ce, &gap_mod_wf);

    feed(&ce, 1u, -40.0f, true, false);
    feed(&ce, 2u, -40.0f, true, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    const uint64_t id = c.candidate_id;
    const uint32_t evidence = c.evidence_count;
    const int64_t last_ev = c.last_evidence_mono_us;
    const uint8_t qsum = c.quality_summary;
    const int spy_before = g_spy_a.execs;
    EXPECT(spy_before >= 2);

    /* Repeated GAP: coast, do not run spy.a, preserve last evidence/quality. */
    feed(&ce, 3u, -40.0f, true, true);
    EXPECT(decision_is(&ce, CE_DECISION_SKIP));
    EXPECT(g_spy_a.execs == spy_before);
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_COASTING);
    EXPECT(c.coast_windows == 1u);
    EXPECT(c.gap_events >= 1u);
    EXPECT(c.evidence_count == evidence);
    EXPECT(c.last_evidence_mono_us == last_ev);
    EXPECT(c.quality_summary == qsum);

    feed(&ce, 4u, -40.0f, true, true);
    EXPECT(decision_is(&ce, CE_DECISION_SKIP));
    EXPECT(g_spy_a.execs == spy_before);
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_COASTING);
    EXPECT(c.coast_windows == 2u);

    /* Recovery after GAP: rematch restores ACTIVE; modules run again. */
    feed(&ce, 5u, -40.0f, true, false);
    EXPECT(ce_find_candidate(&ce, id, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    EXPECT(c.coast_windows == 0u);
    EXPECT(c.evidence_count > evidence);
    EXPECT(c.last_evidence_mono_us == 5000000);
    EXPECT(g_spy_a.execs == spy_before + 1);
    EXPECT(decision_is(&ce, CE_DECISION_KEEP));

    /* Further repeated GAP beyond t_coast → expire. */
    feed(&ce, 6u, -40.0f, true, true);
    feed(&ce, 7u, -40.0f, true, true);
    feed(&ce, 8u, -40.0f, true, true);
    EXPECT(g_spy_a.execs == spy_before + 1);
    EXPECT(ce_candidate_count(&ce) == 0u);
    EXPECT(!ce_find_candidate(&ce, id, &c));
    EXPECT(!ce_promote(&ce, id));
    ce_get_diag(&ce, &d);
    EXPECT(d.quality_gaps >= 5u);
    EXPECT(d.expired >= 1u);
    EXPECT(d.decision_count[CE_DECISION_SKIP] >= 5u);
    ce_stop(&ce);

    /* INVALID: no create, no module, no lifecycle mutation of existing. */
    EXPECT(start_ce(&ce, 2u, 3u));
    reset_spies();
    EXPECT(ce_module_register(&ce, &g_desc_a, &g_spy_a));
    ce_engine_set_workflow(&ce, &gap_mod_wf);
    feed(&ce, 1u, -40.0f, true, false);
    feed(&ce, 2u, -40.0f, true, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    const uint64_t live = c.candidate_id;
    const uint32_t live_ev = c.evidence_count;
    const int spy_live = g_spy_a.execs;
    ae_signature_t bad;
    EXPECT(build_sig(&bad, 3u, -40.0f, true, false, false));
    EXPECT(bad.quality == AE_QUALITY_INVALID);
    ce_engine_on_signature(&ce, &bad);
    EXPECT(decision_is(&ce, CE_DECISION_ERROR));
    EXPECT(g_spy_a.execs == spy_live);
    EXPECT(ce_find_candidate(&ce, live, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    EXPECT(c.evidence_count == live_ev);
    EXPECT(c.last_window_seq == 2u);
    ce_get_diag(&ce, &d);
    EXPECT(d.ignored_unusable == 1u);
    EXPECT(d.created == 1u);
    EXPECT(d.workflow_failed == 0u);
    ce_stop(&ce);

    /* Sequence discontinuity: coast once, then process arrived evidence. */
    EXPECT(start_ce(&ce, 2u, 3u));
    reset_spies();
    EXPECT(ce_module_register(&ce, &g_desc_a, &g_spy_a));
    ce_engine_set_workflow(&ce, &gap_mod_wf);
    feed(&ce, 1u, -40.0f, true, false);
    feed(&ce, 2u, -40.0f, true, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    const uint64_t sid = c.candidate_id;
    const int spy_seq = g_spy_a.execs;
    feed(&ce, 5u, -40.0f, true, false); /* skip 3,4 */
    EXPECT(ce_find_candidate(&ce, sid, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    EXPECT(c.gap_events >= 1u);
    EXPECT(c.last_window_seq == 5u);
    EXPECT(g_spy_a.execs == spy_seq + 1);
    ce_get_diag(&ce, &d);
    EXPECT(d.seq_discontinuities >= 1u);
    EXPECT(decision_is(&ce, CE_DECISION_KEEP));
    ce_stop(&ce);
}

/**
 * Promotion is explicit; modules cannot bypass Decision; terminals blocked.
 */
static void test_promotion_safety_audit(void)
{
    ce_engine_t ce;
    ae_candidate_t c;
    ce_diag_t d;
    reset_spies();
    g_spy_a.force_status = CE_MOD_OK;
    g_spy_a.score = 1.0f;
    g_spy_a.code = 99;

    /* Module-only workflow (no DECIDE PROMOTE) → KEEP, never ENTER. */
    const ce_workflow_step_t mod_only_steps[] = {
        {
            .kind = CE_WK_MODULE,
            .cond = CE_COND_ALWAYS,
            .on_missing = CE_DECISION_ERROR,
            .on_error = CE_DECISION_ERROR,
            .module_id = "spy.a",
        },
    };
    const ce_workflow_t mod_only_wf = {
        .id = "test.mod_only",
        .steps = mod_only_steps,
        .step_count = 1u,
    };

    EXPECT(start_ce(&ce, 2u, 3u));
    g_enter = 0;
    g_exit = 0;
    ce_set_promotion_cb(&ce, on_promo, NULL);
    EXPECT(ce_module_register(&ce, &g_desc_a, &g_spy_a));
    ce_engine_set_workflow(&ce, &mod_only_wf);
    feed(&ce, 1u, -40.0f, true, false);
    feed(&ce, 2u, -40.0f, true, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_ACTIVE);
    EXPECT(!c.promotion_entered);
    EXPECT(g_enter == 0);
    EXPECT(decision_is(&ce, CE_DECISION_KEEP));
    ce_get_diag(&ce, &d);
    EXPECT(d.promote_successes == 0u);
    EXPECT(d.decision_count[CE_DECISION_PROMOTE] == 0u);

    /* Explicit promote once; second call fails; no second ENTER. */
    EXPECT(ce_promote(&ce, c.candidate_id));
    EXPECT(g_enter == 1);
    EXPECT(!ce_promote(&ce, c.candidate_id));
    EXPECT(g_enter == 1);
    ce_get_diag(&ce, &d);
    EXPECT(d.promote_successes == 1u);
    EXPECT(d.promotion_enter_events == 1u);
    const uint64_t pid = c.candidate_id;

    /* Terminal reject: cannot promote again. */
    EXPECT(ce_reject(&ce, pid));
    EXPECT(g_exit == 1);
    EXPECT(!ce_promote(&ce, pid));
    EXPECT(g_enter == 1);
    ce_stop(&ce);

    /* Expired terminal cannot promote. */
    EXPECT(start_ce(&ce, 2u, 1u));
    g_enter = 0;
    ce_set_promotion_cb(&ce, on_promo, NULL);
    feed(&ce, 1u, -40.0f, false, false);
    feed(&ce, 2u, -40.0f, false, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    const uint64_t eid = c.candidate_id;
    feed(&ce, 3u, -40.0f, false, true);
    feed(&ce, 4u, -40.0f, false, true);
    EXPECT(ce_candidate_count(&ce) == 0u);
    EXPECT(!ce_promote(&ce, eid));
    EXPECT(g_enter == 0);
    ce_stop(&ce);

    /* NASCENT promote declined (explicit request still required, and blocked). */
    EXPECT(start_ce(&ce, 3u, 3u));
    g_enter = 0;
    ce_set_promotion_cb(&ce, on_promo, NULL);
    feed(&ce, 1u, -40.0f, false, false);
    EXPECT(ce_get_candidate(&ce, 0, &c));
    EXPECT(c.state == AE_CAND_NASCENT);
    EXPECT(!ce_promote(&ce, c.candidate_id));
    EXPECT(g_enter == 0);
    ce_get_diag(&ce, &d);
    EXPECT(d.promote_attempts >= 1u);
    EXPECT(d.promote_successes == 0u);
    ce_stop(&ce);
}

int main(void)
{
    test_create_associate_and_ids();
    test_two_concurrent();
    test_capacity_evict_and_drop();
    test_expiry_reject_promote_terminal();
    test_invalid_gap_and_ae_contract();
    test_listener_start_stop();
    test_keep_reject_promote_decisions();
    test_module_runtime();
    test_workflow_paths();
    test_fuse_reads_results_not_score();
    test_deterministic_pair();
    test_bridge_explicit_promotion();
    test_stop_with_lifecycle_states();
    test_gap_invalid_semantics();
    test_promotion_safety_audit();
    if (g_failures != 0) {
        fprintf(stderr, "test_ce_architecture: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ce_architecture: ok\n");
    return 0;
}
