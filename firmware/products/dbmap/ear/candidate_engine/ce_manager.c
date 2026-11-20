#include "ce_manager.h"

#include "ae_signature.h"
#include "ce_log.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

_Static_assert((int)AE_CAND_NASCENT == 0, "state_hist index");
_Static_assert((int)AE_CAND_EXPIRED == 5, "state_hist covers states");
_Static_assert((int)AE_CAND_REJECTED == 4, "reject index");

/* Identity weights — RMS reduced vs historical ae_signature_distance. */
#define CE_ID_W_RMS 0.20f
#define CE_ID_W_BAND 0.30f
#define CE_ID_W_DOM_HZ 0.02f
#define CE_ID_W_FLAT 3.0f
#define CE_ID_W_CENT 0.01f
#define CE_ID_W_PAIR 0.25f

#define CE_ID_DIST_INF 1.0e6f

static void fingerprint_init(ae_candidate_t *c, const ae_signature_t *sig)
{
    c->ema_rms_dbfs = sig->rms_dbfs;
    for (uint32_t i = 0; i < AE_BAND_COUNT; i++) {
        c->band_ema[i] = sig->band_dbfs[i];
    }
    c->mic_mask_seen = sig->mic_mask;

    if (sig->spectral_valid) {
        c->ema_dominant_hz = sig->dominant_hz;
        c->ema_flatness = sig->flatness;
        c->ema_centroid_hz = sig->centroid_hz;
        c->spectral_ema_valid = true;
    } else {
        c->ema_dominant_hz = 0.0f;
        c->ema_flatness = 0.0f;
        c->ema_centroid_hz = 0.0f;
        c->spectral_ema_valid = false;
    }

    if (sig->pair_valid) {
        c->ema_delta_rms_db = sig->delta_rms_db;
        c->pair_ema_valid = true;
    } else {
        c->ema_delta_rms_db = 0.0f;
        c->pair_ema_valid = false;
    }
}

static void fingerprint_update(ae_candidate_t *c, const ae_signature_t *sig)
{
    const float a = 0.35f;
    c->ema_rms_dbfs = (1.0f - a) * c->ema_rms_dbfs + a * sig->rms_dbfs;
    for (uint32_t i = 0; i < AE_BAND_COUNT; i++) {
        c->band_ema[i] = (1.0f - a) * c->band_ema[i] + a * sig->band_dbfs[i];
    }
    c->mic_mask_seen = (uint8_t)(c->mic_mask_seen | sig->mic_mask);

    if (sig->spectral_valid) {
        if (c->spectral_ema_valid) {
            c->ema_dominant_hz =
                (1.0f - a) * c->ema_dominant_hz + a * sig->dominant_hz;
            c->ema_flatness = (1.0f - a) * c->ema_flatness + a * sig->flatness;
            c->ema_centroid_hz =
                (1.0f - a) * c->ema_centroid_hz + a * sig->centroid_hz;
        } else {
            c->ema_dominant_hz = sig->dominant_hz;
            c->ema_flatness = sig->flatness;
            c->ema_centroid_hz = sig->centroid_hz;
            c->spectral_ema_valid = true;
        }
    }

    if (sig->pair_valid) {
        if (c->pair_ema_valid) {
            c->ema_delta_rms_db =
                (1.0f - a) * c->ema_delta_rms_db + a * sig->delta_rms_db;
        } else {
            c->ema_delta_rms_db = sig->delta_rms_db;
            c->pair_ema_valid = true;
        }
    }
}

/**
 * CE identity distance. Does not invent zeros for missing spectral/pair.
 * Band comparison uses shape relative to RMS so level changes alone do not
 * dominate identity. Pair ΔRMS is asymmetry only — not spatial direction.
 */
static float identity_distance(const ae_candidate_t *c, const ae_signature_t *sig)
{
    float dist = CE_ID_W_RMS * fabsf(c->ema_rms_dbfs - sig->rms_dbfs);

    for (uint32_t i = 0; i < AE_BAND_COUNT; i++) {
        const float shape_c = c->band_ema[i] - c->ema_rms_dbfs;
        const float shape_s = sig->band_dbfs[i] - sig->rms_dbfs;
        dist += CE_ID_W_BAND * fabsf(shape_c - shape_s);
    }

    if (c->spectral_ema_valid && sig->spectral_valid) {
        dist += CE_ID_W_DOM_HZ * fabsf(c->ema_dominant_hz - sig->dominant_hz);
        dist += CE_ID_W_FLAT * fabsf(c->ema_flatness - sig->flatness);
        dist += CE_ID_W_CENT * fabsf(c->ema_centroid_hz - sig->centroid_hz);
    }

    if (c->pair_ema_valid && sig->pair_valid) {
        dist += CE_ID_W_PAIR * fabsf(c->ema_delta_rms_db - sig->delta_rms_db);
    }

    return dist;
}

static ae_candidate_t *slot_alloc(ce_engine_t *eng)
{
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (!eng->slots[i].in_use) {
            return &eng->slots[i];
        }
    }
    /* Evict weakest NASCENT if any. Equal evidence keeps the earlier slot. */
    ae_candidate_t *weak = NULL;
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        ae_candidate_t *c = &eng->slots[i];
        if (!c->in_use) {
            continue;
        }
        if (c->state == AE_CAND_NASCENT
            && (weak == NULL || c->evidence_count < weak->evidence_count)) {
            weak = c;
        }
    }
    if (weak != NULL) {
        eng->diag.nascent_evicted++;
    }
    return weak;
}

static void clear_last_evidence(ce_engine_t *eng, const ae_candidate_t *c)
{
    if (eng == NULL || c == NULL) {
        return;
    }
    const ptrdiff_t off = c - eng->slots;
    if (off < 0 || (size_t)off >= (size_t)CE_MAX_CANDIDATES) {
        return;
    }
    eng->last_evidence_sig_valid[(size_t)off] = false;
}

static void remember_last_evidence(
    ce_engine_t *eng,
    const ae_candidate_t *c,
    const ae_signature_t *sig
)
{
    if (eng == NULL || c == NULL || sig == NULL) {
        return;
    }
    const ptrdiff_t off = c - eng->slots;
    if (off < 0 || (size_t)off >= (size_t)CE_MAX_CANDIDATES) {
        return;
    }
    eng->last_evidence_sig[(size_t)off] = *sig;
    eng->last_evidence_sig_valid[(size_t)off] = true;
}

static void slot_clear(ce_engine_t *eng, ae_candidate_t *c)
{
    clear_last_evidence(eng, c);
    memset(c, 0, sizeof(*c));
}

static void emit_exit(ce_engine_t *eng, ae_candidate_t *c)
{
    if (!c->promotion_entered) {
        return;
    }
    if (eng->promotion_cb != NULL) {
        eng->promotion_cb(c, CE_PROMOTION_EXIT, eng->promotion_ctx);
        eng->diag.promotion_exit_events++;
    }
    c->promotion_entered = false;
}

static void expire_slot(ce_engine_t *eng, ae_candidate_t *c)
{
    /*
     * EXIT follows promotion_entered, not the live state enum: apply_gap moves
     * PROMOTED → COASTING before coast expiry, so state==PROMOTED would miss EXIT.
     */
    emit_exit(eng, c);
    clear_last_evidence(eng, c);
    c->state = AE_CAND_EXPIRED;
    c->in_use = false;
    eng->diag.expired++;
    CE_LOGI("expire id=%llu", (unsigned long long)c->candidate_id);
}

/**
 * Find best and second-best identity distances among live candidates.
 * Ties for best keep the later slot (distance <= best_d).
 */
static void find_match_ranks(
    ce_engine_t *eng,
    const ae_signature_t *sig,
    ae_candidate_t **best_out,
    float *best_d_out,
    float *second_d_out
)
{
    ae_candidate_t *best = NULL;
    float best_d = CE_ID_DIST_INF;
    float second_d = CE_ID_DIST_INF;

    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        ae_candidate_t *c = &eng->slots[i];
        if (!c->in_use) {
            continue;
        }
        if (c->state == AE_CAND_REJECTED || c->state == AE_CAND_EXPIRED) {
            continue;
        }
        const float d = identity_distance(c, sig);
        if (d <= best_d) {
            if (best != NULL) {
                second_d = best_d;
            }
            best_d = d;
            best = c;
        } else if (d < second_d) {
            second_d = d;
        }
    }

    *best_out = best;
    *best_d_out = best_d;
    *second_d_out = second_d;
}

static void apply_gap(ce_engine_t *eng, const ae_signature_t *sig)
{
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        ae_candidate_t *c = &eng->slots[i];
        if (!c->in_use) {
            continue;
        }
        c->gap_events++;
        if (c->state == AE_CAND_ACTIVE || c->state == AE_CAND_PROMOTED) {
            c->state = AE_CAND_COASTING;
            c->coast_windows = 1u;
            c->last_update_mono_us = sig->window_end_mono_us;
        } else if (c->state == AE_CAND_COASTING) {
            c->coast_windows++;
            c->last_update_mono_us = sig->window_end_mono_us;
            if (c->coast_windows > eng->cfg.t_coast) {
                expire_slot(eng, c);
            }
        } else if (c->state == AE_CAND_NASCENT) {
            /* Gaps kill weak nascent tracks. */
            expire_slot(eng, c);
        }
    }
}

static void update_matched(ce_engine_t *eng, ae_candidate_t *c, const ae_signature_t *sig)
{
    c->last_evidence_mono_us = sig->window_end_mono_us;
    c->last_update_mono_us = sig->window_end_mono_us;
    c->last_window_seq = sig->window_seq;
    c->quality_summary = sig->quality;
    fingerprint_update(c, sig);

    if (c->state == AE_CAND_COASTING) {
        /*
         * Rematch after a temporary gap: restore PROMOTED when this lifecycle
         * already issued ENTER (promotion_entered). Otherwise ordinary ACTIVE.
         * Prevents silent demotion that would allow a second primary promote.
         */
        c->state = c->promotion_entered ? AE_CAND_PROMOTED : AE_CAND_ACTIVE;
        c->coast_windows = 0u;
    }

    if (c->state == AE_CAND_NASCENT) {
        c->evidence_count++;
        if (c->evidence_count >= eng->cfg.n_enter
            && (sig->quality == AE_QUALITY_OK || sig->quality == AE_QUALITY_DEGRADED)) {
            c->state = AE_CAND_ACTIVE;
        }
    } else if (c->state == AE_CAND_ACTIVE || c->state == AE_CAND_PROMOTED) {
        c->evidence_count++;
    }

    /* Simple heuristic confidence — not a classifier posterior. */
    float conf = (float)c->evidence_count / 10.0f;
    if (conf > 1.0f) {
        conf = 1.0f;
    }
    if (sig->quality == AE_QUALITY_DEGRADED) {
        conf *= 0.85f;
    }
    c->confidence = conf;
    remember_last_evidence(eng, c, sig);
}

static ce_ingest_t create_nascent(ce_engine_t *eng, const ae_signature_t *sig)
{
    ce_ingest_t r;
    memset(&r, 0, sizeof(r));
    r.action = CE_INGEST_IGNORED;

    if (sig->quality != AE_QUALITY_OK && sig->quality != AE_QUALITY_DEGRADED) {
        return r;
    }
    if (sig->rms_dbfs < eng->cfg.min_rms_dbfs) {
        eng->diag.ignored_low_energy++;
        return r;
    }

    ae_candidate_t *c = slot_alloc(eng);
    if (c == NULL) {
        eng->diag.create_dropped_full++;
        r.action = CE_INGEST_DROPPED;
        return r;
    }
    slot_clear(eng, c);
    c->in_use = true;
    c->candidate_id = eng->next_id++;
    c->state = AE_CAND_NASCENT;
    c->created_mono_us = sig->window_end_mono_us;
    c->evidence_count = 0u;
    fingerprint_init(c, sig);
    update_matched(eng, c, sig);
    eng->diag.created++;
    r.action = CE_INGEST_CREATED;
    r.candidate_id = c->candidate_id;
    CE_LOGI(
        "create id=%llu seq=%lu q=%u",
        (unsigned long long)c->candidate_id,
        (unsigned long)sig->window_seq,
        (unsigned)sig->quality
    );
    return r;
}

static ae_candidate_t *find_slot(ce_engine_t *eng, uint64_t candidate_id)
{
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        if (eng->slots[i].in_use && eng->slots[i].candidate_id == candidate_id) {
            return &eng->slots[i];
        }
    }
    return NULL;
}

ce_ingest_t ce_manager_ingest(ce_engine_t *eng, const ae_signature_t *sig)
{
    ce_ingest_t r;
    memset(&r, 0, sizeof(r));
    r.action = CE_INGEST_IGNORED;
    if (eng == NULL || !eng->started || sig == NULL) {
        return r;
    }
    if (!ae_signature_usable_for_tracking(sig)) {
        eng->diag.ignored_unusable++;
        r.action = CE_INGEST_UNUSABLE;
        return r;
    }

    /* Ignore duplicate / rewound seq for the global stream. */
    if (sig->window_seq != 0u && sig->window_seq <= eng->last_processed_seq) {
        eng->diag.ignored_duplicate++;
        r.action = CE_INGEST_DUPLICATE;
        return r;
    }

    if (sig->window_seq > eng->last_processed_seq + 1u && eng->last_processed_seq != 0u) {
        apply_gap(eng, sig);
        eng->diag.seq_discontinuities++;
        r.seq_gap = true;
    }

    eng->last_processed_seq = sig->window_seq;

    if (sig->quality == AE_QUALITY_GAP) {
        apply_gap(eng, sig);
        eng->diag.quality_gaps++;
        r.action = CE_INGEST_GAP;
        return r;
    }

    ae_candidate_t *best = NULL;
    float best_d = CE_ID_DIST_INF;
    float second_d = CE_ID_DIST_INF;
    find_match_ranks(eng, sig, &best, &best_d, &second_d);

    const float match_max = eng->cfg.match_distance_max;
    float hyst = eng->cfg.match_hysteresis;
    if (hyst < 0.0f) {
        hyst = 0.0f;
    }
    float amb_delta = eng->cfg.match_ambiguity_delta;
    if (amb_delta < 0.0f) {
        amb_delta = 0.0f;
    }

    if (best != NULL && best_d <= match_max) {
        if (second_d <= match_max && (second_d - best_d) < amb_delta) {
            eng->diag.match_ambiguous++;
        }
        update_matched(eng, best, sig);
        eng->diag.associated++;
        r.action = CE_INGEST_UPDATED;
        r.candidate_id = best->candidate_id;
        return r;
    }

    if (best != NULL && best_d <= match_max + hyst) {
        eng->diag.match_hysteresis_hold++;
        r.action = CE_INGEST_HYSTERESIS;
        r.candidate_id = best->candidate_id;
        return r;
    }

    const bool seq_gap = r.seq_gap;
    r = create_nascent(eng, sig);
    r.seq_gap = seq_gap;
    return r;
}

bool ce_manager_promote(ce_engine_t *eng, uint64_t candidate_id)
{
    if (eng == NULL || !eng->started) {
        return false;
    }
    eng->diag.promote_attempts++;
    ae_candidate_t *c = find_slot(eng, candidate_id);
    if (c == NULL) {
        return false;
    }
    if (c->promotion_entered) {
        /* Already ENTER'd this lifecycle — no duplicate ENTER. */
        return false;
    }
    if (c->state != AE_CAND_ACTIVE && c->state != AE_CAND_COASTING) {
        return false;
    }
    c->state = AE_CAND_PROMOTED;
    {
        /* Engine-monotonic: survives slot clear / candidate_id reuse. */
        uint32_t gen = eng->next_promotion_gen + 1u;
        if (gen == 0u) {
            gen = 1u;
        }
        eng->next_promotion_gen = gen;
        c->promotion_gen = gen;
    }
    c->promotion_entered = true;
    eng->diag.promote_successes++;
    if (eng->promotion_cb != NULL) {
        eng->promotion_cb(c, CE_PROMOTION_ENTER, eng->promotion_ctx);
        eng->diag.promotion_enter_events++;
    }
    CE_LOGI("promote id=%llu", (unsigned long long)c->candidate_id);
    return true;
}

bool ce_manager_unadmit_promotion(
    ce_engine_t *eng,
    uint64_t candidate_id,
    uint32_t promotion_gen
)
{
    if (eng == NULL || !eng->started || candidate_id == 0u || promotion_gen == 0u) {
        return false;
    }
    ae_candidate_t *c = find_slot(eng, candidate_id);
    if (c == NULL || !c->promotion_entered) {
        return false;
    }
    if (c->promotion_gen != promotion_gen) {
        return false;
    }
    emit_exit(eng, c);
    if (c->state == AE_CAND_PROMOTED) {
        c->state = AE_CAND_ACTIVE;
    }
    eng->diag.promotion_unadmit++;
    CE_LOGI(
        "unadmit id=%llu gen=%lu",
        (unsigned long long)c->candidate_id,
        (unsigned long)promotion_gen
    );
    return true;
}

bool ce_manager_reject(ce_engine_t *eng, uint64_t candidate_id)
{
    if (eng == NULL || !eng->started) {
        return false;
    }
    ae_candidate_t *c = find_slot(eng, candidate_id);
    if (c == NULL) {
        return false;
    }
    if (c->state == AE_CAND_REJECTED || c->state == AE_CAND_EXPIRED) {
        return false;
    }
    emit_exit(eng, c);
    clear_last_evidence(eng, c);
    c->state = AE_CAND_REJECTED;
    c->promotion_entered = false;
    c->in_use = false;
    eng->diag.rejected++;
    CE_LOGI("reject id=%llu", (unsigned long long)c->candidate_id);
    return true;
}

void ce_manager_release_for_stop(ce_engine_t *eng)
{
    if (eng == NULL || !eng->started) {
        return;
    }
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        ae_candidate_t *c = &eng->slots[i];
        if (!c->in_use) {
            continue;
        }
        /*
         * EXIT before clear: Bridge association must not survive a stopped CE.
         * Observation open/close is not owned here — only association teardown.
         */
        emit_exit(eng, c);
        slot_clear(eng, c);
        eng->diag.stop_cleared++;
    }
}
