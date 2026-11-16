#include "ce_engine.h"

#include "as_profile.h"
#include "ce_log.h"
#include "ce_manager.h"

#include <string.h>

#define CE_MOD_FLAG_PROFILE_MATCH (1u << 8)

static int slot_index_by_id(const ce_engine_t *eng, uint64_t candidate_id)
{
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (eng->slots[i].in_use && eng->slots[i].candidate_id == candidate_id) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Record as.profile workflow result against the candidate slot.
 * Classification only — never promotes or changes identity state.
 */
static void record_classification(
    ce_engine_t *eng,
    uint64_t candidate_id,
    const ce_workflow_result_t *wf
)
{
    const int idx = slot_index_by_id(eng, candidate_id);
    if (idx < 0) {
        return;
    }

    ce_classification_t *cls = &eng->classifications[idx];
    memset(cls, 0, sizeof(*cls));
    cls->candidate_id = candidate_id;
    cls->valid = true;
    cls->profile_id = AS_PROFILE_ID_NONE;
    cls->acoustic_class = AS_CLASS_UNSPECIFIED;
    cls->gap_events_at_match = eng->slots[idx].gap_events;

    if (wf == NULL) {
        eng->diag.profile_match_misses++;
        return;
    }

    bool saw_profile = false;
    for (uint8_t i = 0; i < wf->result_count; i++) {
        const ce_module_result_t *r = &wf->results[i];
        if (r->module_id == NULL || strcmp(r->module_id, "as.profile") != 0) {
            continue;
        }
        saw_profile = true;
        if (r->status == CE_MOD_OK
            && (r->flags & CE_MOD_FLAG_PROFILE_MATCH) != 0u
            && r->code > 0) {
            cls->matched = true;
            cls->profile_id = (uint16_t)r->code;
            cls->distance = r->score;
            const as_profile_t *p = as_profile_get_by_id(cls->profile_id);
            if (p != NULL) {
                cls->acoustic_class = p->acoustic_class;
            }
            eng->diag.profile_match_hits++;
        } else {
            cls->matched = false;
            cls->distance = r->score;
            eng->diag.profile_match_misses++;
        }
        break;
    }
    if (!saw_profile) {
        eng->diag.profile_match_misses++;
    }
}

void ce_config_set_defaults(ce_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->n_enter = 2u;
    cfg->t_coast = 3u;
    cfg->match_distance_max = 8.0f;
    cfg->match_hysteresis = 2.0f;
    cfg->match_ambiguity_delta = 1.5f;
    cfg->min_rms_dbfs = -80.0f;
}

bool ce_start(ce_engine_t *eng, const ce_config_t *cfg)
{
    if (eng == NULL) {
        return false;
    }
    /*
     * Fresh and reused objects are both memset. Callers that already started
     * this object must ce_stop() first so module shutdown and listener removal
     * run. Reading eng before memset is unsafe on an uninitialized stack object.
     */
    memset(eng, 0, sizeof(*eng));
    if (cfg != NULL) {
        eng->cfg = *cfg;
    } else {
        ce_config_set_defaults(&eng->cfg);
    }
    if (eng->cfg.n_enter == 0u) {
        eng->cfg.n_enter = 1u;
    }
    if (eng->cfg.t_coast == 0u) {
        eng->cfg.t_coast = 1u;
    }
    if (eng->cfg.match_hysteresis < 0.0f) {
        eng->cfg.match_hysteresis = 0.0f;
    }
    if (eng->cfg.match_ambiguity_delta < 0.0f) {
        eng->cfg.match_ambiguity_delta = 0.0f;
    }
    eng->next_id = 1u;
    eng->next_promotion_gen = 0u;
    ce_runtime_reset(&eng->runtime);
    if (!ce_runtime_register(&eng->runtime, ce_module_evidence_desc(), NULL)) {
        return false;
    }
    if (!ce_runtime_register(&eng->runtime, ce_module_profile_desc(), NULL)) {
        ce_runtime_shutdown(&eng->runtime);
        return false;
    }
    if (!ce_runtime_start(&eng->runtime)) {
        ce_runtime_shutdown(&eng->runtime);
        return false;
    }
    eng->workflow = ce_workflow_track();
    eng->started = true;
    return true;
}

void ce_stop(ce_engine_t *eng)
{
    if (eng == NULL || !eng->started) {
        return;
    }
    /* Listener first — no new evidence while releasing promoted lifecycles. */
    ce_unbind_audio_engine(eng);
    ce_manager_release_for_stop(eng);
    ce_runtime_shutdown(&eng->runtime);
    eng->workflow = NULL;
    eng->have_decision = false;
    eng->started = false;
    CE_LOGI("stop");
}

bool ce_bind_audio_engine(ce_engine_t *eng, ae_engine_t *ae)
{
    if (eng == NULL || !eng->started || ae == NULL || !ae->started) {
        return false;
    }
    if (eng->listener_bound && eng->bound_ae == ae) {
        return true;
    }
    if (eng->listener_bound) {
        ce_unbind_audio_engine(eng);
    }
    if (!ae_add_signature_listener(ae, ce_on_signature, eng)) {
        return false;
    }
    eng->bound_ae = ae;
    eng->listener_bound = true;
    return true;
}

void ce_unbind_audio_engine(ce_engine_t *eng)
{
    if (eng == NULL || !eng->listener_bound) {
        return;
    }
    if (eng->bound_ae != NULL) {
        (void)ae_remove_signature_listener(eng->bound_ae, ce_on_signature, eng);
    }
    eng->bound_ae = NULL;
    eng->listener_bound = false;
}

void ce_set_promotion_cb(ce_engine_t *eng, ce_promotion_cb_t cb, void *ctx)
{
    if (eng == NULL) {
        return;
    }
    eng->promotion_cb = cb;
    eng->promotion_ctx = ctx;
}

static void note_decision(ce_engine_t *eng, ce_decision_t d)
{
    if ((unsigned)d >= CE_DECISION_COUNT) {
        d = CE_DECISION_ERROR;
    }
    eng->last_decision = d;
    eng->have_decision = true;
    eng->diag.decision_count[d]++;
}

static void apply_decision(ce_engine_t *eng, uint64_t candidate_id, ce_decision_t d)
{
    note_decision(eng, d);
    if (d == CE_DECISION_PROMOTE) {
        (void)ce_promote(eng, candidate_id);
    } else if (d == CE_DECISION_REJECT) {
        (void)ce_reject(eng, candidate_id);
    }
}

static void run_workflow(ce_engine_t *eng, uint64_t candidate_id, const ae_signature_t *sig)
{
    ae_candidate_t cand;
    const ae_candidate_t *cand_ptr = NULL;
    if (ce_find_candidate(eng, candidate_id, &cand)) {
        cand_ptr = &cand;
    }
    ce_module_input_t input;
    ce_module_input_set(&input, sig, cand_ptr);

    const ce_workflow_t *wf = eng->workflow != NULL ? eng->workflow : ce_workflow_track();
    ce_workflow_result_t result;
    ce_workflow_run(&eng->runtime, wf, &input, &result);
    record_classification(eng, candidate_id, &result);
    if (result.failed) {
        eng->diag.workflow_failed++;
        CE_LOGW(
            "workflow fail id=%llu decision=%s",
            (unsigned long long)candidate_id,
            ce_decision_name(result.decision)
        );
    } else {
        eng->diag.workflow_completed++;
    }
    apply_decision(eng, candidate_id, result.decision);
}

void ce_engine_on_signature(ce_engine_t *eng, const ae_signature_t *sig)
{
    if (eng == NULL || !eng->started || sig == NULL) {
        return;
    }

    const ce_ingest_t in = ce_manager_ingest(eng, sig);
    switch (in.action) {
    case CE_INGEST_UNUSABLE:
        /* Invalid evidence is explicit and does not create or promote. */
        note_decision(eng, CE_DECISION_ERROR);
        return;
    case CE_INGEST_GAP:
        /*
         * A gap is not fed to modules as continuous evidence. The manager
         * already coasted or expired. Completion is SKIP.
         */
        note_decision(eng, CE_DECISION_SKIP);
        eng->diag.workflow_completed++;
        return;
    case CE_INGEST_UPDATED:
    case CE_INGEST_CREATED:
        run_workflow(eng, in.candidate_id, sig);
        return;
    case CE_INGEST_HYSTERESIS:
        /* Near an existing track but outside rematch — no create, no workflow. */
        return;
    case CE_INGEST_IGNORED:
    case CE_INGEST_DUPLICATE:
    case CE_INGEST_DROPPED:
    default:
        return;
    }
}

void ce_on_signature(const ae_signature_t *sig, void *ctx)
{
    ce_engine_on_signature((ce_engine_t *)ctx, sig);
}

uint8_t ce_candidate_count(const ce_engine_t *eng)
{
    if (eng == NULL || !eng->started) {
        return 0u;
    }
    uint8_t n = 0u;
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (eng->slots[i].in_use) {
            n++;
        }
    }
    return n;
}

bool ce_get_candidate(const ce_engine_t *eng, uint8_t index, ae_candidate_t *out)
{
    if (eng == NULL || !eng->started || out == NULL) {
        return false;
    }
    uint8_t seen = 0u;
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (!eng->slots[i].in_use) {
            continue;
        }
        if (seen == index) {
            *out = eng->slots[i];
            return true;
        }
        seen++;
    }
    return false;
}

bool ce_find_candidate(const ce_engine_t *eng, uint64_t id, ae_candidate_t *out)
{
    if (eng == NULL || !eng->started || out == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (eng->slots[i].in_use && eng->slots[i].candidate_id == id) {
            *out = eng->slots[i];
            return true;
        }
    }
    return false;
}

bool ce_promote(ce_engine_t *eng, uint64_t candidate_id)
{
    return ce_manager_promote(eng, candidate_id);
}

bool ce_unadmit_promotion(
    ce_engine_t *eng,
    uint64_t candidate_id,
    uint32_t promotion_gen
)
{
    return ce_manager_unadmit_promotion(eng, candidate_id, promotion_gen);
}

bool ce_reject(ce_engine_t *eng, uint64_t candidate_id)
{
    return ce_manager_reject(eng, candidate_id);
}

void ce_engine_set_workflow(ce_engine_t *eng, const ce_workflow_t *wf)
{
    if (eng == NULL || !eng->started) {
        return;
    }
    eng->workflow = wf != NULL ? wf : ce_workflow_track();
}

bool ce_module_register(ce_engine_t *eng, const ce_module_desc_t *desc, void *ctx)
{
    if (eng == NULL || !eng->started) {
        return false;
    }
    return ce_runtime_register(&eng->runtime, desc, ctx);
}

bool ce_module_is_registered(
    const ce_engine_t *eng,
    const char *module_id,
    uint16_t *version_out
)
{
    if (eng == NULL || !eng->started) {
        return false;
    }
    bool inited = false;
    if (!ce_runtime_module_info(&eng->runtime, module_id, version_out, &inited)) {
        return false;
    }
    return inited;
}

bool ce_engine_last_decision(const ce_engine_t *eng, ce_decision_t *out)
{
    if (eng == NULL || !eng->started || !eng->have_decision || out == NULL) {
        return false;
    }
    *out = eng->last_decision;
    return true;
}

bool ce_get_classification(
    const ce_engine_t *eng,
    uint64_t candidate_id,
    ce_classification_t *out
)
{
    if (eng == NULL || !eng->started || out == NULL) {
        return false;
    }
    const int idx = slot_index_by_id(eng, candidate_id);
    if (idx < 0) {
        return false;
    }
    const ce_classification_t *cls = &eng->classifications[idx];
    if (!cls->valid || cls->candidate_id != candidate_id) {
        return false;
    }
    *out = *cls;
    return true;
}

void ce_get_diag(const ce_engine_t *eng, ce_diag_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (eng == NULL || !eng->started) {
        return;
    }
    *out = eng->diag;
    memset(out->state_hist, 0, sizeof(out->state_hist));
    out->in_use_count = 0u;
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (!eng->slots[i].in_use) {
            continue;
        }
        out->in_use_count++;
        const uint8_t st = (uint8_t)eng->slots[i].state;
        if (st < 6u) {
            out->state_hist[st]++;
        }
    }
    out->module_exec = eng->runtime.exec_total;
    out->module_errors = eng->runtime.exec_errors;
    out->module_skipped = eng->runtime.exec_skipped;
    out->module_missing = eng->runtime.missing;
}
