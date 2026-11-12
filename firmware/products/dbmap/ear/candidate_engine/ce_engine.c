#include "ce_engine.h"

#include "ae_signature.h"

#include <string.h>

void ce_config_set_defaults(ce_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->n_enter = 2u;
    cfg->t_coast = 3u;
    cfg->match_distance_max = 8.0f;
    cfg->min_rms_dbfs = -80.0f;
}

bool ce_start(ce_engine_t *eng, const ce_config_t *cfg)
{
    if (eng == NULL) {
        return false;
    }
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
    eng->next_id = 1u;
    eng->started = true;
    return true;
}

void ce_set_promotion_cb(ce_engine_t *eng, ce_promotion_cb_t cb, void *ctx)
{
    if (eng == NULL) {
        return;
    }
    eng->promotion_cb = cb;
    eng->promotion_ctx = ctx;
}

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
    /* Evict weakest NASCENT if any. */
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

static void expire_slot(ce_engine_t *eng, ae_candidate_t *c)
{
    /*
     * EXIT follows promotion_entered, not the live state enum: apply_gap moves
     * PROMOTED → COASTING before coast expiry, so state==PROMOTED would miss EXIT.
     */
    if (c->promotion_entered) {
        if (eng->promotion_cb != NULL) {
            eng->promotion_cb(c, CE_PROMOTION_EXIT, eng->promotion_ctx);
            eng->diag.promotion_exit_events++;
        }
        c->promotion_entered = false;
    }
    c->state = AE_CAND_EXPIRED;
    c->in_use = false;
    eng->diag.expired++;
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

static void create_nascent(ce_engine_t *eng, const ae_signature_t *sig)
{
    if (sig->quality != AE_QUALITY_OK && sig->quality != AE_QUALITY_DEGRADED) {
        return;
    }
    if (sig->rms_dbfs < eng->cfg.min_rms_dbfs) {
        return;
    }

    ae_candidate_t *c = slot_alloc(eng);
    if (c == NULL) {
        eng->diag.create_dropped_full++;
        return;
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
}

void ce_engine_on_signature(ce_engine_t *eng, const ae_signature_t *sig)
{
    if (eng == NULL || !eng->started || sig == NULL) {
        return;
    }
    if (!ae_signature_usable_for_tracking(sig)) {
        return;
    }

    /* Ignore duplicate / rewound seq for global stream. */
    if (sig->window_seq != 0u && sig->window_seq <= eng->last_processed_seq) {
        return;
    }

    if (sig->window_seq > eng->last_processed_seq + 1u && eng->last_processed_seq != 0u) {
        /* Implicit gap windows before this seq. */
        apply_gap(eng, sig);
    }

    eng->last_processed_seq = sig->window_seq;

    if (sig->quality == AE_QUALITY_GAP) {
        apply_gap(eng, sig);
        return;
    }

    ae_candidate_t *match = best_match(eng, sig);
    if (match != NULL) {
        update_matched(eng, match, sig);
        return;
    }

    create_nascent(eng, sig);
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
    if (eng == NULL || !eng->started) {
        return false;
    }
    eng->diag.promote_attempts++;
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        ae_candidate_t *c = &eng->slots[i];
        if (!c->in_use || c->candidate_id != candidate_id) {
            continue;
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
        return true;
    }
    return false;
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
}
