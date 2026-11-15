#include "ce_manager.h"

#include "ae_signature.h"
#include "ce_log.h"

#include <string.h>

_Static_assert((int)AE_CAND_NASCENT == 0, "state_hist index");
_Static_assert((int)AE_CAND_EXPIRED == 5, "state_hist covers states");
_Static_assert((int)AE_CAND_REJECTED == 4, "reject index");

static void fingerprint_init(ae_candidate_t *c, const ae_signature_t *sig)
{
    c->ema_rms_dbfs = sig->rms_dbfs;
    for (uint32_t i = 0; i < AE_BAND_COUNT; i++) {
        c->band_ema[i] = sig->band_dbfs[i];
    }
    c->mic_mask_seen = sig->mic_mask;
}

static void fingerprint_update(ae_candidate_t *c, const ae_signature_t *sig)
{
    const float a = 0.35f;
    c->ema_rms_dbfs = (1.0f - a) * c->ema_rms_dbfs + a * sig->rms_dbfs;
    for (uint32_t i = 0; i < AE_BAND_COUNT; i++) {
        c->band_ema[i] = (1.0f - a) * c->band_ema[i] + a * sig->band_dbfs[i];
    }
    c->mic_mask_seen = (uint8_t)(c->mic_mask_seen | sig->mic_mask);
}

static float fingerprint_distance(const ae_candidate_t *c, const ae_signature_t *sig)
{
    ae_signature_t proxy;
    memset(&proxy, 0, sizeof(proxy));
    proxy.rms_dbfs = c->ema_rms_dbfs;
    for (uint32_t i = 0; i < AE_BAND_COUNT; i++) {
        proxy.band_dbfs[i] = c->band_ema[i];
    }
    /* Spectral fields unused in fingerprint proxy — distance uses RMS+bands. */
    return ae_signature_distance(&proxy, sig);
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

static void slot_clear(ae_candidate_t *c)
{
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
    c->state = AE_CAND_EXPIRED;
    c->in_use = false;
    eng->diag.expired++;
    CE_LOGI("expire id=%llu", (unsigned long long)c->candidate_id);
}

static ae_candidate_t *best_match(ce_engine_t *eng, const ae_signature_t *sig)
{
    ae_candidate_t *best = NULL;
    float best_d = eng->cfg.match_distance_max;
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        ae_candidate_t *c = &eng->slots[i];
        if (!c->in_use) {
            continue;
        }
        if (c->state == AE_CAND_REJECTED || c->state == AE_CAND_EXPIRED) {
            continue;
        }
        float d = fingerprint_distance(c, sig);
        if (d <= best_d) {
            best_d = d;
            best = c;
        }
    }
    return best;
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
    slot_clear(c);
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

    ae_candidate_t *match = best_match(eng, sig);
    if (match != NULL) {
        update_matched(eng, match, sig);
        eng->diag.associated++;
        r.action = CE_INGEST_UPDATED;
        r.candidate_id = match->candidate_id;
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
    c->promotion_entered = true;
    eng->diag.promote_successes++;
    if (eng->promotion_cb != NULL) {
        eng->promotion_cb(c, CE_PROMOTION_ENTER, eng->promotion_ctx);
        eng->diag.promotion_enter_events++;
    }
    CE_LOGI("promote id=%llu", (unsigned long long)c->candidate_id);
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
        slot_clear(c);
        eng->diag.stop_cleared++;
    }
}
