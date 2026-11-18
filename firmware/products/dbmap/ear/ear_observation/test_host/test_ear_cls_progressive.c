/**
 * Host tests: progressive ALARM taxonomy + OM refinement (no re-promote).
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/candidate_engine/include \
 *     -I firmware/products/dbmap/ear/acoustic_profile/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     -I firmware/products/dbmap/ear/ear_observation/include \
 *     -I firmware/platform/observation/include \
 *     -I firmware/platform/audio/include \
 *     firmware/products/dbmap/ear/audio_engine/ae_engine.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/acoustic_profile/as_profile_as001.c \
 *     firmware/products/dbmap/ear/acoustic_profile/as_profile_match.c \
 *     firmware/products/dbmap/ear/acoustic_profile/as_profile_registry.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_engine.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_decision.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_workflow.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_module_runtime.c \
 *     firmware/products/dbmap/ear/candidate_engine/ce_module_profile.c \
 *     firmware/platform/observation/episode_registry.c \
 *     firmware/products/dbmap/ear/ear_observation/ear_cls_progressive.c \
 *     firmware/products/dbmap/ear/ear_observation/ear_obs_manager.c \
 *     firmware/products/dbmap/ear/ear_observation/ear_promotion_policy.c \
 *     firmware/products/dbmap/ear/ear_observation/test_host/test_ear_cls_progressive.c \
 *     -lm -o /tmp/test_ear_cls_progressive && /tmp/test_ear_cls_progressive
 */
#include "as_profile.h"
#include "ce_engine.h"
#include "ear_cls_progressive.h"
#include "ear_obs_manager.h"
#include "ear_promotion_policy.h"
#include "sp_episode_registry.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;


static void path_join(const ear_cls_assessment_wire_t *w, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0u) {
        return;
    }
    out[0] = '\0';
    if (w == NULL || !w->present || w->n_nodes == 0u) {
        return;
    }
    size_t used = 0u;
    for (uint8_t i = 0u; i < w->n_nodes; i++) {
        const char *id = w->nodes[i].id;
        size_t n = 0u;
        while (id[n] != '\0') {
            n++;
        }
        size_t need = n + (used > 0u ? 1u : 0u);
        if (used + need + 1u > out_len) {
            out[0] = '\0';
            return;
        }
        if (used > 0u) {
            out[used++] = '/';
        }
        for (size_t k = 0u; k < n; k++) {
            out[used++] = id[k];
        }
        out[used] = '\0';
    }
}

static int path_eq(const ear_cls_assessment_wire_t *w, const char *expect)
{
    char buf[96];
    path_join(w, buf, sizeof(buf));
    return strcmp(buf, expect) == 0;
}

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void test_early_alarm(void)
{
    ear_cls_state_t st;
    ear_cls_state_init(&st);
    ear_cls_evidence_t ev;
    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.7f;
    ear_cls_assessment_wire_t wire;
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_START, &wire));
    EXPECT(wire.present);
    EXPECT(path_eq(&wire, "ALARM"));
    EXPECT(st.deepest == EAR_CLS_NODE_ALARM);
}

static void test_refine_alarm_to_se_periodic(void)
{
    ear_cls_state_t st;
    ear_cls_state_init(&st);
    ear_cls_evidence_t ev;
    ear_cls_assessment_wire_t wire;

    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.7f;
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_START, &wire));
    EXPECT(path_eq(&wire, "ALARM"));

    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.7f;
    ev.se_public_alert_ok = true;
    ev.se_public_alert_confidence = 0.8f;
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_PERIODIC, &wire));
    EXPECT(path_eq(&wire, "ALARM/SE_PUBLIC_ALERT"));
}

static void test_refine_to_faran_over(void)
{
    ear_cls_state_t st;
    ear_cls_state_init(&st);
    ear_cls_evidence_t ev;
    ear_cls_assessment_wire_t wire;

    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.9f;
    ev.se_public_alert_ok = true;
    ev.se_public_alert_confidence = 0.85f;
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_START, &wire));

    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.9f;
    ev.se_public_alert_ok = true;
    ev.se_public_alert_confidence = 0.85f;
    ev.faran_over_ok = true;
    ev.faran_over_confidence = 0.8f;
    ev.faran_over_support_ms = 9000u;
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_PERIODIC, &wire));
    EXPECT(path_eq(&wire, "ALARM/SE_PUBLIC_ALERT/FARAN_OVER"));
}

static void test_premature_leaf_blocked(void)
{
    ear_cls_state_t st;
    ear_cls_state_init(&st);
    ear_cls_evidence_t ev;
    ear_cls_assessment_wire_t wire;

    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.9f;
    ev.se_public_alert_ok = true;
    ev.se_public_alert_confidence = 0.85f;
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_PERIODIC, &wire));

    /* Broad siren-like leaf score without duration → stay at SE_PUBLIC_ALERT. */
    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.9f;
    ev.se_public_alert_ok = true;
    ev.se_public_alert_confidence = 0.85f;
    ev.faran_over_ok = true;
    ev.faran_over_confidence = 0.9f;
    ev.faran_over_support_ms = 1000u; /* below LEAF_PERIODIC_MIN_SUPPORT_MS */
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_PERIODIC, &wire));
    EXPECT(path_eq(&wire, "ALARM/SE_PUBLIC_ALERT"));
}

static void test_remain_broad_insufficient(void)
{
    ear_cls_state_t st;
    ear_cls_state_init(&st);
    ear_cls_evidence_t ev;
    ear_cls_assessment_wire_t wire;

    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.55f;
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_START, &wire));
    EXPECT(path_eq(&wire, "ALARM"));

    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.55f;
    ev.se_public_alert_ok = true;
    ev.se_public_alert_confidence = 0.4f; /* below SE min */
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_PERIODIC, &wire));
    EXPECT(path_eq(&wire, "ALARM"));
}

static void test_independent_per_node_confidence(void)
{
    ear_cls_state_t st;
    ear_cls_state_init(&st);
    ear_cls_evidence_t ev;
    ear_cls_assessment_wire_t wire;

    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.95f;
    ev.se_public_alert_ok = true;
    ev.se_public_alert_confidence = 0.88f;
    ev.faran_over_ok = true;
    ev.faran_over_confidence = 0.76f;
    ev.faran_over_support_ms = 9000u;
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_PERIODIC, &wire));
    EXPECT(wire.n_nodes == 3u);
    EXPECT(strcmp(wire.nodes[0].id, "ALARM") == 0);
    EXPECT(wire.nodes[0].confidence > 0.94f && wire.nodes[0].confidence < 0.96f);
    EXPECT(strcmp(wire.nodes[1].id, "SE_PUBLIC_ALERT") == 0);
    EXPECT(wire.nodes[1].confidence > 0.87f && wire.nodes[1].confidence < 0.89f);
    EXPECT(strcmp(wire.nodes[2].id, "FARAN_OVER") == 0);
    EXPECT(wire.nodes[2].confidence > 0.75f && wire.nodes[2].confidence < 0.77f);
    /* Depth and confidence are independent: deepest conf can be lower. */
    EXPECT(wire.nodes[2].confidence < wire.nodes[0].confidence);
}

static void test_no_sibling_swap_on_periodic(void)
{
    ear_cls_state_t st;
    ear_cls_state_init(&st);
    ear_cls_evidence_t ev;
    ear_cls_assessment_wire_t wire;

    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.9f;
    ev.se_public_alert_ok = true;
    ev.se_public_alert_confidence = 0.9f;
    ev.vma_ok = true;
    ev.vma_confidence = 0.8f;
    ev.vma_support_ms = 10000u;
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_PERIODIC, &wire));
    EXPECT(path_eq(&wire, "ALARM/SE_PUBLIC_ALERT/VMA"));

    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.9f;
    ev.se_public_alert_ok = true;
    ev.se_public_alert_confidence = 0.9f;
    ev.faran_over_ok = true;
    ev.faran_over_confidence = 0.99f;
    ev.faran_over_support_ms = 60000u;
    ev.vma_ok = true;
    ev.vma_confidence = 0.8f;
    ev.vma_support_ms = 10000u;
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_PERIODIC, &wire));
    EXPECT(path_eq(&wire, "ALARM/SE_PUBLIC_ALERT/VMA"));
    EXPECT(st.correction_count == 0u);
}

static void test_sibling_correction_at_end_only(void)
{
    ear_cls_state_t st;
    ear_cls_state_init(&st);
    ear_cls_evidence_t ev;
    ear_cls_assessment_wire_t wire;

    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.9f;
    ev.se_public_alert_ok = true;
    ev.se_public_alert_confidence = 0.9f;
    ev.vma_ok = true;
    ev.vma_confidence = 0.76f;
    ev.vma_support_ms = 12000u;
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_PERIODIC, &wire));
    EXPECT(st.leaf == EAR_CLS_NODE_VMA);

    /* Strong FARAN_OVER contest — still no swap on PERIODIC. */
    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.9f;
    ev.se_public_alert_ok = true;
    ev.se_public_alert_confidence = 0.9f;
    ev.vma_ok = true;
    ev.vma_confidence = 0.76f;
    ev.vma_support_ms = 12000u;
    ev.faran_over_ok = true;
    ev.faran_over_confidence = 0.96f; /* margin > EAR_CLS_END_CORR_MARGIN */
    ev.faran_over_support_ms = 30000u; /* >= 1.5 * 12000 and >= 20000 */
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_PERIODIC, &wire));
    EXPECT(st.leaf == EAR_CLS_NODE_VMA);

    /* END with required stronger/longer evidence → correction. */
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_END, &wire));
    EXPECT(st.leaf == EAR_CLS_NODE_FARAN_OVER);
    EXPECT(st.correction_count == 1u);
    EXPECT(path_eq(&wire, "ALARM/SE_PUBLIC_ALERT/FARAN_OVER"));
}

static void test_weak_end_correction_rejected(void)
{
    ear_cls_state_t st;
    ear_cls_state_init(&st);
    ear_cls_evidence_t ev;
    ear_cls_assessment_wire_t wire;

    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.9f;
    ev.se_public_alert_ok = true;
    ev.se_public_alert_confidence = 0.9f;
    ev.vma_ok = true;
    ev.vma_confidence = 0.8f;
    ev.vma_support_ms = 15000u;
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_PERIODIC, &wire));

    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.9f;
    ev.se_public_alert_ok = true;
    ev.se_public_alert_confidence = 0.9f;
    ev.vma_ok = true;
    ev.vma_confidence = 0.8f;
    ev.vma_support_ms = 15000u;
    ev.faran_over_ok = true;
    ev.faran_over_confidence = 0.86f; /* margin vs 0.8 = 0.06 < 0.20 */
    ev.faran_over_support_ms = 25000u;
    EXPECT(ear_cls_apply(&st, &ev, EAR_CLS_PHASE_END, &wire));
    EXPECT(st.leaf == EAR_CLS_NODE_VMA);
    EXPECT(st.correction_count == 0u);
}

/* --- OM integration: same observation_id, no second promote --- */

typedef struct {
    uint32_t starts;
    uint32_t periodics;
    uint32_t ends;
    char start_obs[SP_OBS_EPISODE_ID_LEN];
    char last_obs[SP_OBS_EPISODE_ID_LEN];
    char last_family[96];
} cls_emit_t;

static cls_emit_t g_emit;
static uint8_t g_uuid_n;

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
    memcpy(g_emit.start_obs, binding->observation_id, SP_OBS_EPISODE_ID_LEN);
    memcpy(g_emit.last_obs, binding->observation_id, SP_OBS_EPISODE_ID_LEN);
    g_emit.last_family[0] = '\0';
    if (binding->assessment.present) {
        path_join(&binding->assessment, g_emit.last_family, sizeof(g_emit.last_family));
    }
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
    memcpy(g_emit.last_obs, binding->observation_id, SP_OBS_EPISODE_ID_LEN);
    if (binding->assessment.present) {
        path_join(&binding->assessment, g_emit.last_family, sizeof(g_emit.last_family));
    }
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
    (void)reason;
    (void)interval_start_mono_us;
    (void)interval_end_mono_us;
    (void)has_signal_level_dbfs;
    (void)signal_level_dbfs;
    g_emit.ends++;
    memcpy(g_emit.last_obs, binding->observation_id, SP_OBS_EPISODE_ID_LEN);
    if (binding->assessment.present) {
        path_join(&binding->assessment, g_emit.last_family, sizeof(g_emit.last_family));
    }
    return true;
}

static void fake_uuid(char out[SP_OBS_EPISODE_ID_LEN])
{
    snprintf(
        out,
        SP_OBS_EPISODE_ID_LEN,
        "aaaaaaaa-bbbb-4ccc-8ddd-%012u",
        (unsigned)g_uuid_n++
    );
}

static ae_candidate_t *plant(
    ce_engine_t *ce,
    uint64_t id,
    ae_cand_state_t state,
    uint32_t evidence,
    float ema
)
{
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (ce->slots[i].in_use) {
            continue;
        }
        memset(&ce->slots[i], 0, sizeof(ce->slots[i]));
        ce->slots[i].in_use = true;
        ce->slots[i].candidate_id = id;
        ce->slots[i].state = state;
        ce->slots[i].evidence_count = evidence;
        ce->slots[i].ema_rms_dbfs = ema;
        ce->slots[i].quality_summary = AE_QUALITY_OK;
        ce->slots[i].last_evidence_mono_us = (int64_t)id * 1000;
        if (id >= ce->next_id) {
            ce->next_id = id + 1u;
        }
        return &ce->slots[i];
    }
    EXPECT(0 && "no free CE slot");
    return NULL;
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

static bool promote_with_reserve(
    ear_obs_manager_t *om,
    ce_engine_t *ce,
    uint64_t cid,
    int64_t now_us
)
{
    mark_as001(ce, cid);
    ear_obs_rsv_handle_t h;
    if (ear_obs_manager_reserve(om, &h) != EAR_OBS_RSV_OK) {
        return false;
    }
    if (!ear_obs_manager_bind_reservation_candidate(om, h, cid)) {
        ear_obs_manager_release_reservation(om, h);
        return false;
    }
    if (!ear_obs_manager_stamp_reservation_relevance(
            om, h, cid, AS_PROFILE_ID_AS001, 1.0f
        )) {
        ear_obs_manager_release_reservation(om, h);
        return false;
    }
    if (!ce_promote(ce, cid)) {
        ear_obs_manager_release_reservation(om, h);
        return false;
    }
    ear_obs_manager_poll(om, now_us);
    return true;
}

static void test_om_refine_same_observation_id(void)
{
    memset(&g_emit, 0, sizeof(g_emit));
    g_uuid_n = 1u;

    ae_engine_t ae;
    ce_engine_t ce;
    sp_episode_registry_t reg;
    ear_obs_manager_t om;

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

    plant(&ce, 42u, AE_CAND_ACTIVE, 5u, -20.0f);

    ear_cls_evidence_t ev;
    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.7f;
    ear_obs_manager_arm_cls_evidence(&om, &ev);

    EXPECT(promote_with_reserve(&om, &ce, 42u, 1000));
    EXPECT(g_emit.starts == 1u);
    EXPECT(strcmp(g_emit.last_family, "ALARM") == 0);
    char obs0[SP_OBS_EPISODE_ID_LEN];
    memcpy(obs0, g_emit.start_obs, sizeof(obs0));

    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.75f;
    ev.se_public_alert_ok = true;
    ev.se_public_alert_confidence = 0.8f;
    EXPECT(ear_obs_manager_feed_cls_evidence(
        &om, 42u, &ev, EAR_CLS_PHASE_PERIODIC
    ));
    ear_obs_manager_on_activity_periodic_hint(&om, 1000, 6000, true, -30.0f);
    EXPECT(g_emit.periodics == 1u);
    EXPECT(strcmp(g_emit.last_obs, obs0) == 0);
    EXPECT(strcmp(g_emit.last_family, "ALARM/SE_PUBLIC_ALERT") == 0);
    EXPECT(ear_obs_manager_n_open(&om) == 1u);

    ear_cls_evidence_clear(&ev);
    ev.alarm_ok = true;
    ev.alarm_confidence = 0.9f;
    ev.se_public_alert_ok = true;
    ev.se_public_alert_confidence = 0.9f;
    ev.faran_over_ok = true;
    ev.faran_over_confidence = 0.8f;
    ev.faran_over_support_ms = 9000u;
    EXPECT(ear_obs_manager_feed_cls_evidence(
        &om, 42u, &ev, EAR_CLS_PHASE_PERIODIC
    ));
    ear_obs_manager_on_activity_periodic_hint(&om, 6000, 11000, true, -30.0f);
    EXPECT(strcmp(g_emit.last_obs, obs0) == 0);
    EXPECT(strcmp(g_emit.last_family, "ALARM/SE_PUBLIC_ALERT/FARAN_OVER") == 0);
    EXPECT(g_emit.starts == 1u);

    ce_stop(&ce);
}

int main(void)
{
    test_early_alarm();
    test_refine_alarm_to_se_periodic();
    test_refine_to_faran_over();
    test_premature_leaf_blocked();
    test_remain_broad_insufficient();
    test_independent_per_node_confidence();
    test_no_sibling_swap_on_periodic();
    test_sibling_correction_at_end_only();
    test_weak_end_correction_rejected();
    test_om_refine_same_observation_id();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
