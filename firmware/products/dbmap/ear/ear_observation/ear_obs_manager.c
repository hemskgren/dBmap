/**
 * EAR Observation Manager — promotion-gated Observation lifecycle.
 *
 * Single-task serialization (device activity_task / host tests). No lock.
 */
#include "ear_obs_manager.h"

#include "ae_engine.h"
#include "as_profile.h"
#include "ce_engine.h"
#include "ear_cls_as001.h"

#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#endif

/*
 * Event-level START-anchor diagnostics only (no behavior change).
 * Enabled with the existing opt-in UART serial-diag Kconfig used on lab images.
 */
#if defined(ESP_PLATFORM) && defined(CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG) \
    && CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
#define EAR_OBS_MGR_START_ANCHOR_DIAG 1
static const char *TAG = "ear_obs";
#else
#define EAR_OBS_MGR_START_ANCHOR_DIAG 0
#endif

static uint32_t as002_absence_hold_ms(const ear_obs_manager_t *om)
{
    if (om == NULL || om->cfg.as002_signature_absence_hold_ms == 0u) {
        return EAR_OBS_AS002_SIGNATURE_ABSENCE_HOLD_MS_DEFAULT;
    }
    return om->cfg.as002_signature_absence_hold_ms;
}

static uint32_t detached_end_timeout_ms(const ear_obs_manager_t *om)
{
    if (om == NULL || om->cfg.detached_end_timeout_ms == 0u) {
        return EAR_OBS_DETACHED_END_TIMEOUT_MS_DEFAULT;
    }
    return om->cfg.detached_end_timeout_ms;
}

static bool binding_is_as002(const ear_obs_manager_t *om, uint8_t bi)
{
    if (om == NULL || bi >= EAR_OBS_MGR_BINDING_CAP || !om->bind[bi].in_use) {
        return false;
    }
    return om->bind[bi].snap.product_relevance_approved
        && om->bind[bi].snap.relevance_profile_id == AS_PROFILE_ID_AS002;
}

static void clear_detached_watchdog(ear_obs_manager_t *om, uint8_t bi, bool count_cancel)
{
    if (om == NULL || bi >= EAR_OBS_MGR_BINDING_CAP) {
        return;
    }
    if (om->bind[bi].detached_since_mono_us != 0) {
        om->bind[bi].detached_since_mono_us = 0;
        if (count_cancel) {
            om->diag.detached_watchdog_cancelled++;
        }
    }
}

/**
 * Arm Stage 3 watchdog when ce_live transitions true→false on a non-AS002
 * binding. Idempotent arm uses the transition edge only (caller checks).
 */
static void arm_detached_watchdog(
    ear_obs_manager_t *om,
    uint8_t bi,
    int64_t now_mono_us
)
{
    if (om == NULL || bi >= EAR_OBS_MGR_BINDING_CAP || !om->bind[bi].in_use) {
        return;
    }
    if (binding_is_as002(om, bi)) {
        return;
    }
    if (now_mono_us <= 0) {
        now_mono_us = 1;
    }
    om->bind[bi].detached_since_mono_us = now_mono_us;
    om->diag.detached_watchdog_armed++;
}

#define EAR_OBS_RSV_MAGIC 0xE0B50001u
#define EAR_OBS_PRODUCER_MGR 2u

static uint8_t count_held(const ear_obs_manager_t *om)
{
    uint8_t n = 0u;
    for (uint8_t i = 0u; i < EAR_OBS_MGR_RSV_SLOTS; i++) {
        if (om->rsv[i].state == EAR_OBS_RSV_ST_HELD) {
            n++;
        }
    }
    return n;
}

static uint8_t count_open(const ear_obs_manager_t *om)
{
    uint8_t n = 0u;
    for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (om->bind[i].in_use) {
            n++;
        }
    }
    return n;
}

uint8_t ear_obs_manager_n_open(const ear_obs_manager_t *om)
{
    return om == NULL ? 0u : count_open(om);
}

uint8_t ear_obs_manager_n_pending(const ear_obs_manager_t *om)
{
    return om == NULL ? 0u : om->pending_count;
}

uint8_t ear_obs_manager_n_held(const ear_obs_manager_t *om)
{
    return om == NULL ? 0u : count_held(om);
}

bool ear_obs_manager_has_signature_observation(
    const ear_obs_manager_t *om,
    uint16_t relevance_profile_id
)
{
    if (om == NULL || relevance_profile_id == 0u) {
        return false;
    }
    for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (!om->bind[i].in_use) {
            continue;
        }
        if (om->bind[i].snap.product_relevance_approved
            && om->bind[i].snap.relevance_profile_id == relevance_profile_id) {
            return true;
        }
    }
    for (uint8_t i = 0u; i < EAR_OBS_MGR_PENDING_CAP; i++) {
        if (!om->pending[i].in_use || om->pending[i].cancelled) {
            continue;
        }
        if (om->pending[i].snap.product_relevance_approved
            && om->pending[i].snap.relevance_profile_id == relevance_profile_id) {
            return true;
        }
    }
    return false;
}

void ear_obs_manager_get_diag(const ear_obs_manager_t *om, ear_obs_manager_diag_t *out)
{
    if (out == NULL) {
        return;
    }
    if (om == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = om->diag;
}

void ear_obs_manager_init(ear_obs_manager_t *om, const ear_obs_manager_config_t *cfg)
{
    if (om == NULL) {
        return;
    }
    memset(om, 0, sizeof(*om));
    if (cfg != NULL) {
        om->cfg = *cfg;
    }
    if (om->cfg.pending_timeout_ms == 0u) {
        om->cfg.pending_timeout_ms = EAR_OBS_PENDING_TIMEOUT_MS_DEFAULT;
    }
    if (om->cfg.observation_interval_ms == 0u) {
        om->cfg.observation_interval_ms = 5000u;
    }
    if (om->cfg.producer_tag == 0u) {
        om->cfg.producer_tag = EAR_OBS_PRODUCER_MGR;
    }
    if (om->cfg.as002_signature_absence_hold_ms == 0u) {
        om->cfg.as002_signature_absence_hold_ms =
            EAR_OBS_AS002_SIGNATURE_ABSENCE_HOLD_MS_DEFAULT;
    }
    if (om->cfg.detached_end_timeout_ms == 0u) {
        om->cfg.detached_end_timeout_ms = EAR_OBS_DETACHED_END_TIMEOUT_MS_DEFAULT;
    }
}

static bool rsv_handle_valid(
    const ear_obs_manager_t *om,
    ear_obs_rsv_handle_t h,
    ear_obs_rsv_state_t expect
)
{
    if (om == NULL || h.magic != EAR_OBS_RSV_MAGIC) {
        return false;
    }
    if (h.index >= EAR_OBS_MGR_RSV_SLOTS) {
        return false;
    }
    const ear_obs_rsv_state_t st = om->rsv[h.index].state;
    if (st == EAR_OBS_RSV_ST_NONE) {
        return false;
    }
    if (om->rsv[h.index].generation != h.generation) {
        return false;
    }
    if (expect != EAR_OBS_RSV_ST_NONE && st != expect) {
        return false;
    }
    return true;
}

static void clear_rsv_slot(ear_obs_manager_t *om, uint8_t index)
{
    uint16_t gen = (uint16_t)(om->rsv[index].generation + 1u);
    if (gen == 0u) {
        gen = 1u;
    }
    memset(&om->rsv[index], 0, sizeof(om->rsv[index]));
    om->rsv[index].generation = gen;
    om->rsv[index].state = EAR_OBS_RSV_ST_NONE;
}

ear_obs_rsv_result_t ear_obs_manager_reserve(
    ear_obs_manager_t *om,
    ear_obs_rsv_handle_t *out
)
{
    if (om == NULL || out == NULL) {
        return EAR_OBS_RSV_INVALID;
    }
    memset(out, 0, sizeof(*out));

    const uint8_t held = count_held(om);
    const uint8_t pending = om->pending_count;
    if ((uint16_t)held + (uint16_t)pending >= EAR_OBS_MGR_PENDING_CAP) {
        om->diag.reserve_full++;
        return EAR_OBS_RSV_FULL;
    }

    for (uint8_t i = 0u; i < EAR_OBS_MGR_RSV_SLOTS; i++) {
        if (om->rsv[i].state != EAR_OBS_RSV_ST_NONE) {
            continue;
        }
        uint16_t gen = om->rsv[i].generation;
        if (gen == 0u) {
            gen = 1u;
        }
        memset(&om->rsv[i], 0, sizeof(om->rsv[i]));
        om->rsv[i].state = EAR_OBS_RSV_ST_HELD;
        om->rsv[i].generation = gen;
        om->rsv[i].held_mono_us = 0;
        out->magic = EAR_OBS_RSV_MAGIC;
        out->index = i;
        out->generation = gen;
        om->diag.reserve_ok++;
        return EAR_OBS_RSV_OK;
    }

    om->diag.reserve_full++;
    return EAR_OBS_RSV_FULL;
}

bool ear_obs_manager_bind_reservation_candidate(
    ear_obs_manager_t *om,
    ear_obs_rsv_handle_t h,
    uint64_t candidate_id
)
{
    if (!rsv_handle_valid(om, h, EAR_OBS_RSV_ST_HELD) || candidate_id == 0u) {
        return false;
    }
    om->rsv[h.index].expected_candidate_id = candidate_id;
    om->rsv[h.index].has_expected_id = true;
    return true;
}

bool ear_obs_manager_stamp_reservation_relevance(
    ear_obs_manager_t *om,
    ear_obs_rsv_handle_t h,
    uint64_t candidate_id,
    uint16_t profile_id,
    float match_distance
)
{
    if (!rsv_handle_valid(om, h, EAR_OBS_RSV_ST_HELD) || candidate_id == 0u) {
        return false;
    }
    if (!om->rsv[h.index].has_expected_id
        || om->rsv[h.index].expected_candidate_id != candidate_id) {
        return false;
    }
    uint32_t ticket = om->next_relevance_ticket + 1u;
    if (ticket == 0u) {
        ticket = 1u;
    }
    om->next_relevance_ticket = ticket;
    om->rsv[h.index].relevance_approved = true;
    om->rsv[h.index].relevance_profile_id = profile_id;
    om->rsv[h.index].relevance_match_distance = match_distance;
    om->rsv[h.index].relevance_ticket = ticket;
    return true;
}

static void request_ce_unadmit(
    ear_obs_manager_t *om,
    uint64_t candidate_id,
    uint32_t promotion_gen
)
{
    if (om == NULL || om->cfg.ce == NULL || candidate_id == 0u
        || promotion_gen == 0u) {
        return;
    }
    if (ce_unadmit_promotion(om->cfg.ce, candidate_id, promotion_gen)) {
        om->diag.promote_unadmit++;
    }
}

void ear_obs_manager_release_reservation(
    ear_obs_manager_t *om,
    ear_obs_rsv_handle_t h
)
{
    if (!rsv_handle_valid(om, h, EAR_OBS_RSV_ST_HELD)) {
        return;
    }
    clear_rsv_slot(om, (uint8_t)h.index);
    om->diag.reserve_released++;
}

static int find_binding(const ear_obs_manager_t *om, uint64_t candidate_id)
{
    for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (om->bind[i].in_use && om->bind[i].candidate_id == candidate_id) {
            return (int)i;
        }
    }
    return -1;
}

static int find_pending(const ear_obs_manager_t *om, uint64_t candidate_id)
{
    for (uint8_t i = 0u; i < EAR_OBS_MGR_PENDING_CAP; i++) {
        if (om->pending[i].in_use && !om->pending[i].cancelled
            && om->pending[i].candidate_id == candidate_id) {
            return (int)i;
        }
    }
    return -1;
}

static int find_held_for_candidate(const ear_obs_manager_t *om, uint64_t candidate_id)
{
    for (uint8_t i = 0u; i < EAR_OBS_MGR_RSV_SLOTS; i++) {
        if (om->rsv[i].state == EAR_OBS_RSV_ST_HELD
            && om->rsv[i].has_expected_id
            && om->rsv[i].expected_candidate_id == candidate_id) {
            return (int)i;
        }
    }
    return -1;
}

bool ear_obs_manager_binding_ce_live(const ear_obs_manager_t *om, uint64_t candidate_id)
{
    const int bi = find_binding(om, candidate_id);
    return bi >= 0 && om->bind[bi].ce_live;
}

static void fill_binding_view(
    const ear_obs_manager_t *om,
    uint8_t bi,
    ear_obs_binding_view_t *out
)
{
    memset(out, 0, sizeof(*out));
    out->candidate_id = om->bind[bi].candidate_id;
    out->ep = om->bind[bi].ep;
    memcpy(out->observation_id, om->bind[bi].observation_id, sizeof(out->observation_id));
    out->ce_live = om->bind[bi].ce_live;
    out->start_mono_us = om->bind[bi].start_mono_us;
    out->assessment = om->bind[bi].cls_wire;
}

static void lifecycle_lock(ear_obs_manager_t *om)
{
    if (om->cfg.lifecycle_lock != NULL) {
        om->cfg.lifecycle_lock(om->cfg.lifecycle_lock_ctx);
    }
}

static void lifecycle_unlock(ear_obs_manager_t *om)
{
    if (om->cfg.lifecycle_unlock != NULL) {
        om->cfg.lifecycle_unlock(om->cfg.lifecycle_lock_ctx);
    }
}

static bool slot_start_accepted(const ear_obs_manager_t *om, sp_ep_handle_t ep)
{
    if (om == NULL || om->cfg.registry == NULL) {
        return false;
    }
    const sp_episode_slot_t *slot = sp_episode_registry_get_const(om->cfg.registry, ep);
    return slot != NULL && slot->start_publish_accepted;
}

static void fill_frozen_start_view(
    const ear_obs_manager_t *om,
    uint8_t bi,
    ear_obs_binding_view_t *out
)
{
    fill_binding_view(om, bi, out);
    out->assessment = om->bind[bi].start_assessment;
}

static void freeze_start_inputs(ear_obs_manager_t *om, uint8_t bi, int64_t now_mono_us)
{
    if (om->bind[bi].start_frozen) {
        return;
    }
    om->bind[bi].start_snap = om->bind[bi].snap;
    om->bind[bi].start_assessment = om->bind[bi].cls_wire;
    om->bind[bi].start_capture_mono_us = now_mono_us;
    om->bind[bi].start_frozen = true;
}

/**
 * One START publication. Does not allocate, promote, or change generation.
 * Failed attempts keep the binding and schedule a single later retry.
 */
static void attempt_start_publish(
    ear_obs_manager_t *om,
    uint8_t bi,
    int64_t attempt_now_us
)
{
    if (om == NULL || bi >= EAR_OBS_MGR_BINDING_CAP || !om->bind[bi].in_use) {
        return;
    }
    if (om->cfg.registry == NULL) {
        return;
    }
    if (!sp_episode_registry_handle_valid(om->cfg.registry, om->bind[bi].ep)) {
        return;
    }
    if (slot_start_accepted(om, om->bind[bi].ep)) {
        return;
    }

    freeze_start_inputs(om, bi, attempt_now_us);

    lifecycle_lock(om);
    if (!om->bind[bi].in_use
        || !sp_episode_registry_handle_valid(om->cfg.registry, om->bind[bi].ep)
        || slot_start_accepted(om, om->bind[bi].ep)) {
        lifecycle_unlock(om);
        return;
    }

    ear_obs_binding_view_t view;
    fill_frozen_start_view(om, bi, &view);
    bool ok = true;
    if (om->cfg.emit_start != NULL) {
        ok = om->cfg.emit_start(
            om->cfg.emit_ctx,
            &view,
            &om->bind[bi].start_snap,
            om->bind[bi].start_capture_mono_us
        );
    }
    if (ok) {
        sp_episode_registry_mark_start_accepted(
            om->cfg.registry,
            om->bind[bi].ep,
            om->bind[bi].start_capture_mono_us
        );
    } else {
        om->bind[bi].start_retry_not_before_mono_us =
            attempt_now_us + EAR_OBS_START_RETRY_INTERVAL_US;
    }
    lifecycle_unlock(om);
}

/** At most one retry per due binding. No catch-up loop. */
static void retry_unaccepted_starts(ear_obs_manager_t *om, int64_t now_mono_us)
{
    for (uint8_t bi = 0u; bi < EAR_OBS_MGR_BINDING_CAP; bi++) {
        if (!om->bind[bi].in_use || !om->bind[bi].start_frozen) {
            continue;
        }
        if (om->cfg.registry == NULL) {
            continue;
        }
        if (!sp_episode_registry_handle_valid(om->cfg.registry, om->bind[bi].ep)) {
            continue;
        }
        if (slot_start_accepted(om, om->bind[bi].ep)) {
            continue;
        }
        if (om->bind[bi].start_retry_not_before_mono_us == 0) {
            continue;
        }
        if (now_mono_us < om->bind[bi].start_retry_not_before_mono_us) {
            continue;
        }
        om->diag.start_retry++;
        attempt_start_publish(om, bi, now_mono_us);
    }
}

static void note_binding_released(ear_obs_manager_t *om, uint8_t bi)
{
    if (om->cfg.on_binding_released == NULL) {
        return;
    }
    om->cfg.on_binding_released(
        om->cfg.emit_ctx,
        om->bind[bi].observation_id,
        om->bind[bi].ep.generation
    );
}

/** Deepen AS-001 AIRCRAFT → Helicopter before PERIODIC/END emit (not START). */
static void as001_maybe_deepen(
    ear_obs_manager_t *om,
    uint8_t bi,
    ear_cls_phase_t phase
)
{
    if (om == NULL || bi >= EAR_OBS_MGR_BINDING_CAP || !om->bind[bi].in_use) {
        return;
    }
    if (om->bind[bi].snap.relevance_profile_id != AS_PROFILE_ID_AS001) {
        return;
    }
    if (phase == EAR_CLS_PHASE_START) {
        return;
    }
    (void)ear_cls_as001_apply(
        &om->bind[bi].cls_wire,
        om->bind[bi].snap.relevance_match_distance,
        phase
    );
}

static bool allocate_and_bind(
    ear_obs_manager_t *om,
    uint64_t candidate_id,
    const ear_obs_promote_snapshot_t *snap,
    int64_t now_mono_us
)
{
    if (om->cfg.registry == NULL || om->cfg.uuid_fn == NULL || snap == NULL) {
        return false;
    }
    /* pending/open invariant: never bind without product-relevance stamp. */
    if (!snap->product_relevance_approved || snap->relevance_ticket == 0u) {
        om->diag.relevance_reject++;
        return false;
    }
    if (count_open(om) >= EAR_OBS_MGR_BINDING_CAP) {
        return false;
    }
    if (find_binding(om, candidate_id) >= 0) {
        return false;
    }

    int free_bi = -1;
    for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (!om->bind[i].in_use) {
            free_bi = (int)i;
            break;
        }
    }
    if (free_bi < 0) {
        return false;
    }

    sp_ep_handle_t ep;
    if (!sp_episode_registry_allocate(
            om->cfg.registry,
            &ep,
            now_mono_us,
            om->cfg.observation_interval_ms,
            om->cfg.producer_tag,
            om->cfg.uuid_fn,
            false
        )) {
        return false;
    }

    const sp_episode_slot_t *slot =
        sp_episode_registry_get_const(om->cfg.registry, ep);
    if (slot == NULL) {
        sp_episode_registry_release(om->cfg.registry, ep);
        return false;
    }

    memset(&om->bind[free_bi], 0, sizeof(om->bind[free_bi]));
    om->bind[free_bi].in_use = true;
    om->bind[free_bi].ce_live = true;
    om->bind[free_bi].candidate_id = candidate_id;
    om->bind[free_bi].ce_promotion_gen = snap->promotion_gen;
    /* Fresh binding must not inherit a prior slot's detached timestamp. */
    om->bind[free_bi].detached_since_mono_us = 0;
    om->bind[free_bi].ep = ep;
    memcpy(
        om->bind[free_bi].observation_id,
        slot->observation_id,
        sizeof(om->bind[free_bi].observation_id)
    );
    om->bind[free_bi].start_mono_us = now_mono_us;
    om->bind[free_bi].last_emit_mono_us = now_mono_us;
    om->bind[free_bi].snap = *snap;
    om->bind[free_bi].last_owned_sig_window_seq = 0u;
    if (snap->sig_count > 0u) {
        /* ENTER seeds newest-first (ae_history_copy_newest → sigs[0]). */
        om->bind[free_bi].last_owned_sig_window_seq = snap->sigs[0].window_seq;
    }
    ear_cls_state_init(&om->bind[free_bi].cls);
    memset(&om->bind[free_bi].cls_wire, 0, sizeof(om->bind[free_bi].cls_wire));
    om->bind[free_bi].as002_sig_track =
        (snap->product_relevance_approved
         && snap->relevance_profile_id == AS_PROFILE_ID_AS002);
    om->bind[free_bi].as002_absent_ms = 0u;
    /*
     * Assessment stamp at START (ear_hyp_v2 via cls_wire):
     *   - AS-001: parent-only AIRCRAFT (leaf Helicopter on first PERIODIC/END).
     *     Armed siren evidence is discarded, not applied.
     *   - AS-002: progressive path from armed siren evidence when present.
     * Armed progressive evidence is consumed only by an approved AS-002 binding.
     */
    if (snap->relevance_profile_id == AS_PROFILE_ID_AS001) {
        (void)ear_cls_as001_apply(
            &om->bind[free_bi].cls_wire,
            snap->relevance_match_distance,
            EAR_CLS_PHASE_START
        );
        if (om->cls_arm_valid) {
            om->cls_arm_valid = false;
            ear_cls_evidence_clear(&om->cls_arm);
        }
    } else if (binding_is_as002(om, (uint8_t)free_bi) && om->cls_arm_valid) {
        (void)ear_cls_apply(
            &om->bind[free_bi].cls,
            &om->cls_arm,
            EAR_CLS_PHASE_START,
            &om->bind[free_bi].cls_wire
        );
        om->cls_arm_valid = false;
        ear_cls_evidence_clear(&om->cls_arm);
    }

    attempt_start_publish(om, (uint8_t)free_bi, now_mono_us);

    om->diag.promote_commit_bound++;
    return true;
}

static void pending_remove_at(ear_obs_manager_t *om, uint8_t order_pos)
{
    if (order_pos >= om->pending_count) {
        return;
    }
    const uint8_t slot = om->pending_order[order_pos];
    for (uint8_t i = order_pos; i + 1u < om->pending_count; i++) {
        om->pending_order[i] = om->pending_order[i + 1u];
    }
    om->pending_count--;
    memset(&om->pending[slot], 0, sizeof(om->pending[slot]));
    om->diag.pending_dequeue++;
}

static bool pending_enqueue(
    ear_obs_manager_t *om,
    uint64_t candidate_id,
    const ear_obs_promote_snapshot_t *snap,
    int64_t entered_mono_us,
    ear_obs_rsv_handle_t rsv_h
)
{
    /* Invariant: pending => product_relevance_approved (+ non-zero ticket). */
    if (snap == NULL || !snap->product_relevance_approved
        || snap->relevance_ticket == 0u) {
        om->diag.relevance_reject++;
        return false;
    }
    if (om->pending_count >= EAR_OBS_MGR_PENDING_CAP) {
        return false;
    }
    if (find_pending(om, candidate_id) >= 0) {
        return false;
    }

    int free_pi = -1;
    for (uint8_t i = 0u; i < EAR_OBS_MGR_PENDING_CAP; i++) {
        if (!om->pending[i].in_use) {
            free_pi = (int)i;
            break;
        }
    }
    if (free_pi < 0) {
        return false;
    }

    memset(&om->pending[free_pi], 0, sizeof(om->pending[free_pi]));
    om->pending[free_pi].in_use = true;
    om->pending[free_pi].cancelled = false;
    om->pending[free_pi].candidate_id = candidate_id;
    om->pending[free_pi].entered_mono_us = entered_mono_us;
    om->pending[free_pi].deadline_mono_us =
        entered_mono_us + (int64_t)om->cfg.pending_timeout_ms * 1000LL;
    om->pending[free_pi].snap = *snap;
    om->pending[free_pi].rsv_h = rsv_h;

    /* Insert by (entered_mono_us asc, candidate_id asc). */
    uint8_t insert_at = om->pending_count;
    for (uint8_t i = 0u; i < om->pending_count; i++) {
        const uint8_t oi = om->pending_order[i];
        const int64_t t = om->pending[oi].entered_mono_us;
        const uint64_t cid = om->pending[oi].candidate_id;
        if (entered_mono_us < t
            || (entered_mono_us == t && candidate_id < cid)) {
            insert_at = i;
            break;
        }
    }
    for (uint8_t i = om->pending_count; i > insert_at; i--) {
        om->pending_order[i] = om->pending_order[i - 1u];
    }
    om->pending_order[insert_at] = (uint8_t)free_pi;
    om->pending_count++;
    om->diag.pending_enqueue++;
    om->diag.promote_commit_pending++;
    return true;
}

static void drain_pending_to_open(ear_obs_manager_t *om, int64_t now_mono_us)
{
    om->diag.drain_attempts++;
    while (om->pending_count > 0u
           && count_open(om) < EAR_OBS_MGR_BINDING_CAP
           && om->cfg.registry != NULL) {
        const uint8_t head = om->pending_order[0];
        if (!om->pending[head].in_use || om->pending[head].cancelled) {
            pending_remove_at(om, 0u);
            continue;
        }

        /* Registry full? stop without skipping head. */
        if (sp_episode_registry_in_use_count(om->cfg.registry) >= SP_OBS_EPISODE_SLOTS) {
            om->diag.drain_alloc_fail++;
            break;
        }

        const uint64_t cid = om->pending[head].candidate_id;
        ear_obs_promote_snapshot_t snap = om->pending[head].snap;

        /*
         * Drain validates the original ENTER stamp only — never invents
         * product-relevance approval for an otherwise unapproved candidate.
         */
        if (!snap.product_relevance_approved) {
            om->diag.relevance_reject++;
            pending_remove_at(om, 0u);
            continue;
        }

        if (!allocate_and_bind(om, cid, &snap, now_mono_us)) {
            om->diag.drain_alloc_fail++;
            break;
        }
        pending_remove_at(om, 0u);
    }
}

bool ear_obs_manager_commit_reservation(
    ear_obs_manager_t *om,
    ear_obs_rsv_handle_t h,
    uint64_t candidate_id,
    const ear_obs_promote_snapshot_t *snapshot
)
{
    if (!rsv_handle_valid(om, h, EAR_OBS_RSV_ST_HELD) || snapshot == NULL
        || candidate_id == 0u) {
        om->diag.promote_commit_fail++;
        return false;
    }
    if (!om->rsv[h.index].has_expected_id
        || om->rsv[h.index].expected_candidate_id != candidate_id) {
        om->diag.promote_commit_fail++;
        return false;
    }
    /* Duplicate ENTER protection. */
    if (find_binding(om, candidate_id) >= 0 || find_pending(om, candidate_id) >= 0) {
        clear_rsv_slot(om, (uint8_t)h.index);
        om->diag.enter_dup++;
        return true;
    }
    if (!snapshot->product_relevance_approved
        || snapshot->relevance_ticket == 0u
        || !om->rsv[h.index].relevance_approved
        || om->rsv[h.index].relevance_ticket != snapshot->relevance_ticket) {
        const uint32_t gen = snapshot->promotion_gen;
        clear_rsv_slot(om, (uint8_t)h.index);
        om->diag.relevance_reject++;
        om->diag.promote_commit_fail++;
        request_ce_unadmit(om, candidate_id, gen);
        return false;
    }

    const int64_t now_us =
        snapshot->promote_mono_us != 0 ? snapshot->promote_mono_us
                                      : snapshot->last_evidence_mono_us;

    /* Own the snapshot before releasing HELD. */
    ear_obs_promote_snapshot_t owned = *snapshot;
    owned.candidate_id = candidate_id;
    owned.product_relevance_approved = true;
    owned.relevance_ticket = snapshot->relevance_ticket;

    /* Consume HELD before open/pending transition. */
    clear_rsv_slot(om, (uint8_t)h.index);

    drain_pending_to_open(om, now_us);

    if (count_open(om) < EAR_OBS_MGR_BINDING_CAP
        && om->cfg.registry != NULL
        && sp_episode_registry_in_use_count(om->cfg.registry) < SP_OBS_EPISODE_SLOTS) {
        if (allocate_and_bind(om, candidate_id, &owned, now_us)) {
            return true;
        }
        /* Fall through to pending if allocate raced/failed. */
    }

    if (!pending_enqueue(om, candidate_id, &owned, now_us, h)) {
        om->diag.promote_commit_fail++;
        request_ce_unadmit(om, candidate_id, owned.promotion_gen);
        return false;
    }
    drain_pending_to_open(om, now_us);
    return true;
}

static bool evq_push(
    ear_obs_manager_t *om,
    uint64_t candidate_id,
    ce_promotion_event_t ev,
    const ae_candidate_t *c
)
{
    if (om->evq_count >= EAR_OBS_MGR_EVENT_Q) {
        return false;
    }
    const uint8_t idx =
        (uint8_t)((om->evq_head + om->evq_count) % EAR_OBS_MGR_EVENT_Q);
    om->evq[idx].candidate_id = candidate_id;
    om->evq[idx].ev = ev;
    om->evq[idx].have_cand = false;
    if (c != NULL) {
        om->evq[idx].cand_copy = *c;
        om->evq[idx].have_cand = true;
    }
    om->evq_count++;
    return true;
}

void ear_obs_manager_on_promotion(
    const ae_candidate_t *c,
    ce_promotion_event_t ev,
    void *ctx
)
{
    ear_obs_manager_t *om = (ear_obs_manager_t *)ctx;
    if (om == NULL || c == NULL) {
        return;
    }
    if (!evq_push(om, c->candidate_id, ev, c)) {
        om->diag.evq_dropped++;
        /*
         * ENTER never reached the queue: release HELD and ask CE to unadmit
         * this promotion_gen so the candidate is not stuck PROMOTED.
         */
        if (ev == CE_PROMOTION_ENTER) {
            const int ri = find_held_for_candidate(om, c->candidate_id);
            if (ri >= 0) {
                clear_rsv_slot(om, (uint8_t)ri);
                om->diag.reserve_released++;
            }
            request_ce_unadmit(om, c->candidate_id, c->promotion_gen);
        }
    }
}

void ear_obs_manager_fill_snapshot_from_candidate(
    ear_obs_manager_t *om,
    const ae_candidate_t *c,
    int64_t now_mono_us,
    ear_obs_promote_snapshot_t *out
)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (c == NULL) {
        return;
    }
    out->candidate_id = c->candidate_id;
    out->promote_mono_us = now_mono_us;
    out->last_evidence_mono_us = c->last_evidence_mono_us;
    out->last_window_seq = c->last_window_seq;
    out->evidence_count = c->evidence_count;
    out->ema_rms_dbfs = c->ema_rms_dbfs;
    memcpy(out->band_ema, c->band_ema, sizeof(out->band_ema));
    out->quality_summary = c->quality_summary;
    out->state_at_promote = (uint8_t)c->state;
    out->gap_events = c->gap_events;
    out->promotion_gen = c->promotion_gen;
    out->sig_count = 0u;
    out->product_relevance_approved = false;
    out->relevance_profile_id = 0u;
    out->relevance_match_distance = 0.0f;
    out->relevance_ticket = 0u;
    (void)om;

    if (om != NULL && om->cfg.ae != NULL) {
        ae_signature_t tmp[EAR_OBS_PROMOTE_SIG_CAP];
        const uint32_t n = ae_history_copy(om->cfg.ae, tmp, EAR_OBS_PROMOTE_SIG_CAP);
        if (n > 0u) {
            const uint32_t copy_n =
                n > EAR_OBS_PROMOTE_SIG_CAP ? EAR_OBS_PROMOTE_SIG_CAP : n;
            memcpy(out->sigs, tmp, sizeof(ae_signature_t) * copy_n);
            out->sig_count = (uint8_t)copy_n;
        }
    }
}

/**
 * Terminate one open binding exactly once.
 * Marks the slot unused before emit/release so re-entrant END/EXIT cannot
 * double-emit or double-release. When defer_drain is true, pending admission
 * is left to the caller (needed when ending multiple bindings in one hint).
 */
static void end_binding(
    ear_obs_manager_t *om,
    uint8_t bi,
    ear_obs_end_reason_t reason,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs,
    bool defer_drain
)
{
    if (bi >= EAR_OBS_MGR_BINDING_CAP || !om->bind[bi].in_use) {
        return;
    }

    int64_t iv_start = interval_start_mono_us;
    int64_t iv_end = interval_end_mono_us;
    if (iv_end <= 0) {
        iv_end = om->bind[bi].last_emit_mono_us;
        if (iv_end <= 0) {
            iv_end = om->bind[bi].start_mono_us;
        }
    }
    if (iv_start <= 0 || iv_start > iv_end) {
        const int64_t fallback =
            (int64_t)om->cfg.observation_interval_ms * 1000LL;
        iv_start = iv_end - fallback;
        if (iv_start < om->bind[bi].start_mono_us) {
            iv_start = om->bind[bi].start_mono_us;
        }
    }

    const sp_ep_handle_t ep = om->bind[bi].ep;
    const bool have_ep =
        om->cfg.registry != NULL
        && sp_episode_registry_handle_valid(om->cfg.registry, ep);
    const bool publish_end = have_ep && slot_start_accepted(om, ep);

    /* Deepen AS-001 leaf before a published END if START was parent-only. */
    if (publish_end) {
        as001_maybe_deepen(om, bi, EAR_CLS_PHASE_END);
    }

    ear_obs_binding_view_t view;
    fill_binding_view(om, bi, &view);
    note_binding_released(om, bi);

    /* Terminal transition first: subsequent callbacks see an empty slot. */
    memset(&om->bind[bi], 0, sizeof(om->bind[bi]));
    om->diag.end_done++;

    if (publish_end && om->cfg.emit_end != NULL) {
        (void)om->cfg.emit_end(
            om->cfg.emit_ctx,
            &view,
            reason,
            iv_start,
            iv_end,
            has_signal_level_dbfs,
            signal_level_dbfs
        );
    }

    if (have_ep) {
        sp_episode_registry_release(om->cfg.registry, ep);
    }

    if (!defer_drain) {
        drain_pending_to_open(om, iv_end);
    }
}

static int find_binding_by_observation_id(
    const ear_obs_manager_t *om,
    const char *observation_id
)
{
    if (om == NULL || observation_id == NULL || observation_id[0] == '\0') {
        return -1;
    }
    for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (om->bind[i].in_use
            && strncmp(
                   om->bind[i].observation_id,
                   observation_id,
                   SP_OBS_EPISODE_ID_LEN
               ) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Cancel pending for candidate_id. Returns true if a live pending was removed
 * (capacity released once). Does not call ce_reject.
 */
static bool cancel_pending_for_candidate(
    ear_obs_manager_t *om,
    uint64_t candidate_id,
    bool from_exit
)
{
    int pi = -1;
    uint8_t order_pos = 0u;
    for (uint8_t i = 0u; i < om->pending_count; i++) {
        const uint8_t slot = om->pending_order[i];
        if (om->pending[slot].in_use && !om->pending[slot].cancelled
            && om->pending[slot].candidate_id == candidate_id) {
            pi = (int)slot;
            order_pos = i;
            break;
        }
    }
    if (pi < 0) {
        return false;
    }

    om->pending[pi].cancelled = true;
    pending_remove_at(om, order_pos);
    if (from_exit) {
        om->diag.pending_cancel_exit++;
    }
    return true;
}

static void handle_enter(ear_obs_manager_t *om, uint64_t candidate_id, const ae_candidate_t *c)
{
    const int existing_bi = find_binding(om, candidate_id);
    if (existing_bi >= 0) {
        /*
         * Same candidate already has an open Observation. If EXIT detached it
         * (e.g. unadmit then re-promote), re-attach ce_live and cancel the
         * Stage 3 watchdog — do not open a second episode.
         */
        if (!om->bind[existing_bi].ce_live) {
            om->bind[existing_bi].ce_live = true;
            if (c != NULL && c->promotion_gen != 0u) {
                om->bind[existing_bi].ce_promotion_gen = c->promotion_gen;
                om->bind[existing_bi].snap.promotion_gen = c->promotion_gen;
            }
            clear_detached_watchdog(om, (uint8_t)existing_bi, true);
            /* Drop any HELD reservation for this re-promote (no second Obs). */
            const int held_ri = find_held_for_candidate(om, candidate_id);
            if (held_ri >= 0) {
                clear_rsv_slot(om, (uint8_t)held_ri);
                om->diag.reserve_released++;
            }
            om->diag.enter_reattach++;
            return;
        }
        om->diag.enter_dup++;
        return;
    }
    if (find_pending(om, candidate_id) >= 0) {
        om->diag.enter_dup++;
        return;
    }

    const int ri = find_held_for_candidate(om, candidate_id);
    if (ri < 0) {
        om->diag.enter_no_rsv++;
        return;
    }

    ear_obs_rsv_handle_t h = {
        .magic = EAR_OBS_RSV_MAGIC,
        .index = (uint16_t)ri,
        .generation = om->rsv[ri].generation,
    };

    ear_obs_promote_snapshot_t snap;
    const bool sel_from_evidence =
        (c != NULL && c->last_evidence_mono_us != 0);
    const int64_t now_us =
        sel_from_evidence ? c->last_evidence_mono_us
                          : om->rsv[ri].held_mono_us;
    if (c != NULL) {
        ear_obs_manager_fill_snapshot_from_candidate(om, c, now_us, &snap);
    } else if (om->rsv[ri].has_snap) {
        snap = om->rsv[ri].snap;
    } else {
        memset(&snap, 0, sizeof(snap));
        snap.candidate_id = candidate_id;
        snap.promote_mono_us = now_us;
    }

    /* Carry Policy ticket from HELD reservation — never re-query classification. */
    if (om->rsv[ri].relevance_approved && om->rsv[ri].relevance_ticket != 0u) {
        snap.product_relevance_approved = true;
        snap.relevance_profile_id = om->rsv[ri].relevance_profile_id;
        snap.relevance_match_distance = om->rsv[ri].relevance_match_distance;
        snap.relevance_ticket = om->rsv[ri].relevance_ticket;
    } else {
        snap.product_relevance_approved = false;
        snap.relevance_ticket = 0u;
    }

#if EAR_OBS_MGR_START_ANCHOR_DIAG
    /*
     * start_anchor enter: records what handle_enter selects as Observation
     * START mono. esp_timer_get_time() is wall-mono at ENTER *handling*
     * (OM event-queue drain). CE does not currently stamp a dedicated
     * promotion callback wall time; snap.promote_mono_us is filled from
     * the same selected now_us (last_evidence or held_mono), not a
     * separate clock. held_mono_us is currently never assigned a real
     * reserve time (stays 0 after reserve).
     */
    ESP_LOGI(
        TAG,
        "start_anchor enter cand=%llu promo_gen=%lu "
        "clock_now_us=%lld sel_src=%s selected_start_mono_us=%lld "
        "last_evidence_mono_us=%lld held_mono_us=%lld "
        "snap_promote_mono_us=%lld last_window_seq=%lu evidence_count=%lu "
        "ema_rms_dbfs=%.2f cand_state=%u",
        (unsigned long long)candidate_id,
        (unsigned long)(c != NULL ? c->promotion_gen : 0u),
        (long long)esp_timer_get_time(),
        sel_from_evidence ? "last_evidence" : "held_mono",
        (long long)now_us,
        (long long)(c != NULL ? c->last_evidence_mono_us : 0),
        (long long)om->rsv[ri].held_mono_us,
        (long long)snap.promote_mono_us,
        (unsigned long)(c != NULL ? c->last_window_seq : snap.last_window_seq),
        (unsigned long)(c != NULL ? c->evidence_count : snap.evidence_count),
        (double)(c != NULL ? c->ema_rms_dbfs : snap.ema_rms_dbfs),
        (unsigned)(c != NULL ? (unsigned)c->state : 0u)
    );
#endif

    (void)ear_obs_manager_commit_reservation(om, h, candidate_id, &snap);
}

static void handle_exit(
    ear_obs_manager_t *om,
    uint64_t candidate_id,
    int64_t now_mono_us,
    bool have_promotion_gen,
    uint32_t promotion_gen
)
{
    /*
     * CE EXIT is detach-only. It never emits Observation END.
     * Identity is (candidate_id, promotion_gen): a delayed EXIT from an older
     * ENTER must not detach or otherwise mutate a newer Observation that
     * reused the same candidate_id (or the same binding slot).
     * Generation comes from the queued cand_copy taken at CE callback time —
     * never from a later candidate_id lookup.
     */
    const int bi = find_binding(om, candidate_id);
    if (bi >= 0) {
        const uint32_t bound_gen = om->bind[bi].ce_promotion_gen;
        /*
         * Gen-tagged binding: EXIT must carry the matching non-zero
         * promotion_gen. Zero-gen / missing / mismatched EXIT never falls
         * back to candidate_id-only detach (production safety).
         */
        if (bound_gen != 0u) {
            if (!have_promotion_gen
                || promotion_gen == 0u
                || promotion_gen != bound_gen) {
                om->diag.exit_stale++;
                return;
            }
        }
        if (om->bind[bi].ce_live) {
            om->bind[bi].ce_live = false;
            om->diag.exit_detach++;
            /* Stage 3: arm detached watchdog (non-AS002 only). */
            arm_detached_watchdog(om, (uint8_t)bi, now_mono_us);
        }
    }

    /* Cancel pending without ce_reject. Idempotent if already cancelled. */
    if (cancel_pending_for_candidate(om, candidate_id, true)) {
        drain_pending_to_open(om, now_mono_us);
    }

    /* Release any matching HELD reservation (orphan promote path). */
    const int ri = find_held_for_candidate(om, candidate_id);
    if (ri >= 0) {
        clear_rsv_slot(om, (uint8_t)ri);
        om->diag.reserve_released++;
    }
}

static void timeout_pending_at(
    ear_obs_manager_t *om,
    uint8_t order_pos,
    int64_t now_mono_us
)
{
    if (order_pos >= om->pending_count) {
        return;
    }
    const uint8_t slot = om->pending_order[order_pos];
    if (!om->pending[slot].in_use || om->pending[slot].cancelled) {
        return;
    }

    const uint64_t cid = om->pending[slot].candidate_id;
    om->pending[slot].cancelled = true;
    pending_remove_at(om, order_pos);
    om->diag.pending_timeout++;

    /* After local cleanup, reject CE if still findable. EXIT must be idempotent. */
    if (om->cfg.ce != NULL) {
        ae_candidate_t live;
        if (ce_find_candidate(om->cfg.ce, cid, &live)) {
            if (live.state != AE_CAND_REJECTED && live.state != AE_CAND_EXPIRED) {
                (void)ce_reject(om->cfg.ce, cid);
            }
        }
    }

    drain_pending_to_open(om, now_mono_us);
}

static void process_event_queue(ear_obs_manager_t *om, int64_t now_mono_us)
{
    while (om->evq_count > 0u) {
        const uint8_t idx = om->evq_head;
        const uint64_t cid = om->evq[idx].candidate_id;
        const ce_promotion_event_t ev = om->evq[idx].ev;
        ae_candidate_t cand_copy;
        const bool have = om->evq[idx].have_cand;
        if (have) {
            cand_copy = om->evq[idx].cand_copy;
        }
        om->evq_head = (uint8_t)((om->evq_head + 1u) % EAR_OBS_MGR_EVENT_Q);
        om->evq_count--;

        if (ev == CE_PROMOTION_ENTER) {
            handle_enter(om, cid, have ? &cand_copy : NULL);
        } else if (ev == CE_PROMOTION_EXIT) {
            handle_exit(
                om,
                cid,
                now_mono_us,
                have,
                have ? cand_copy.promotion_gen : 0u
            );
        }
    }
}

static bool sig_admissible_for_obs_evidence(const ae_signature_t *sig)
{
    if (sig == NULL) {
        return false;
    }
    if (sig->window_seq == 0u || sig->window_end_mono_us <= 0) {
        return false;
    }
    /* Observation evidence is continuous usable windows only — not GAP/INVALID. */
    if (sig->quality != AE_QUALITY_OK && sig->quality != AE_QUALITY_DEGRADED) {
        return false;
    }
    return true;
}

void ear_obs_manager_on_candidate_evidence(
    ear_obs_manager_t *om,
    uint64_t candidate_id,
    const ae_signature_t *sig_copy,
    const ae_candidate_t *cand_copy
)
{
    if (om == NULL || candidate_id == 0u) {
        return;
    }
    const int bi = find_binding(om, candidate_id);
    if (bi < 0 || !om->bind[bi].ce_live) {
        om->diag.evidence_dropped++;
        return;
    }

    /*
     * Stale promotion_gen must not refresh a detached/rebound Observation.
     * Zero on either side is treated as unset (legacy / synthetic callers).
     */
    if (cand_copy != NULL
        && om->bind[bi].ce_promotion_gen != 0u
        && cand_copy->promotion_gen != 0u
        && cand_copy->promotion_gen != om->bind[bi].ce_promotion_gen) {
        om->diag.evidence_dropped++;
        return;
    }

    if (cand_copy != NULL) {
        om->bind[bi].snap.last_evidence_mono_us = cand_copy->last_evidence_mono_us;
        om->bind[bi].snap.last_window_seq = cand_copy->last_window_seq;
        om->bind[bi].snap.evidence_count = cand_copy->evidence_count;
        om->bind[bi].snap.ema_rms_dbfs = cand_copy->ema_rms_dbfs;
        memcpy(
            om->bind[bi].snap.band_ema,
            cand_copy->band_ema,
            sizeof(om->bind[bi].snap.band_ema)
        );
        om->bind[bi].snap.quality_summary = cand_copy->quality_summary;
        om->bind[bi].snap.gap_events = cand_copy->gap_events;
    }

    if (sig_copy != NULL) {
        bool admit = sig_admissible_for_obs_evidence(sig_copy);
        if (admit && cand_copy != NULL
            && sig_copy->window_seq != cand_copy->last_window_seq) {
            /* Must be the signature that updated this candidate — not AE latest. */
            admit = false;
        }
        if (admit
            && sig_copy->window_seq <= om->bind[bi].last_owned_sig_window_seq) {
            /* Duplicate or out-of-order relative to owned evidence. */
            admit = false;
        }
        if (!admit) {
            om->diag.evidence_sig_rejected++;
        } else {
            /* Shift ring of owned signatures; newest at [0]; drop oldest if full. */
            for (uint8_t i = EAR_OBS_PROMOTE_SIG_CAP - 1u; i > 0u; i--) {
                om->bind[bi].snap.sigs[i] = om->bind[bi].snap.sigs[i - 1u];
            }
            om->bind[bi].snap.sigs[0] = *sig_copy;
            if (om->bind[bi].snap.sig_count < EAR_OBS_PROMOTE_SIG_CAP) {
                om->bind[bi].snap.sig_count++;
            }
            om->bind[bi].last_owned_sig_window_seq = sig_copy->window_seq;
        }
    }

    om->bind[bi].evidence_updates++;
    om->diag.evidence_accepted++;
}

void ear_obs_manager_feed_from_ce(ear_obs_manager_t *om, ce_engine_t *ce)
{
    if (om == NULL || ce == NULL || !ce->started) {
        return;
    }
    for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (!om->bind[i].in_use || !om->bind[i].ce_live) {
            continue;
        }
        ae_candidate_t live;
        if (!ce_find_candidate(ce, om->bind[i].candidate_id, &live)) {
            continue;
        }
        ae_signature_t sig;
        const ae_signature_t *sigp = NULL;
        if (ce_get_last_evidence_signature(ce, live.candidate_id, &sig)) {
            sigp = &sig;
        }
        ear_obs_manager_on_candidate_evidence(
            om, live.candidate_id, sigp, &live
        );
    }
}

void ear_obs_manager_on_activity_end_hint(
    ear_obs_manager_t *om,
    ear_obs_activity_end_kind_t kind,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs
)
{
    if (om == NULL) {
        return;
    }
    om->diag.end_hint++;
    /*
     * Snapshot observation_ids first so deferred pending drain cannot cause a
     * newly admitted Observation to be ended in the same hint.
     */
    char obs_ids[EAR_OBS_MGR_BINDING_CAP][SP_OBS_EPISODE_ID_LEN];
    uint8_t n_ids = 0u;
    for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (!om->bind[i].in_use) {
            continue;
        }
        memcpy(
            obs_ids[n_ids],
            om->bind[i].observation_id,
            SP_OBS_EPISODE_ID_LEN
        );
        n_ids++;
    }

    const bool end_as002 = (kind == EAR_OBS_ACT_END_INVALID_HOLD);

    for (uint8_t j = 0u; j < n_ids; j++) {
        const int bi = find_binding_by_observation_id(om, obs_ids[j]);
        if (bi < 0) {
            continue;
        }
        if (!end_as002 && binding_is_as002(om, (uint8_t)bi)) {
            om->diag.activity_end_as002_skipped++;
            continue;
        }
        end_binding(
            om,
            (uint8_t)bi,
            EAR_OBS_END_HINT_ACTIVITY,
            interval_start_mono_us,
            interval_end_mono_us,
            has_signal_level_dbfs,
            signal_level_dbfs,
            true /* defer_drain */
        );
    }

    drain_pending_to_open(
        om,
        interval_end_mono_us > 0 ? interval_end_mono_us : interval_start_mono_us
    );
}

void ear_obs_manager_on_as002_siren_window(
    ear_obs_manager_t *om,
    bool usable,
    bool tonal_vote,
    uint32_t window_ms,
    int64_t window_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs
)
{
    if (om == NULL) {
        return;
    }

    const uint32_t hold_ms = as002_absence_hold_ms(om);
    const uint32_t win_ms = window_ms > 0u ? window_ms : 1000u;

    /* Snapshot ids: ending one AS002 must not disturb iteration if capacity grows. */
    char obs_ids[EAR_OBS_MGR_BINDING_CAP][SP_OBS_EPISODE_ID_LEN];
    uint8_t n_ids = 0u;
    for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (!om->bind[i].in_use || !om->bind[i].as002_sig_track) {
            continue;
        }
        memcpy(
            obs_ids[n_ids],
            om->bind[i].observation_id,
            SP_OBS_EPISODE_ID_LEN
        );
        n_ids++;
    }

    for (uint8_t j = 0u; j < n_ids; j++) {
        const int bi = find_binding_by_observation_id(om, obs_ids[j]);
        if (bi < 0 || !om->bind[bi].as002_sig_track) {
            continue;
        }

        if (!usable) {
            om->diag.as002_sig_absent_pause++;
            continue;
        }

        if (tonal_vote) {
            om->bind[bi].as002_absent_ms = 0u;
            om->diag.as002_sig_support_reset++;
            continue;
        }

        /* Usable non-tonal: advance absence; never use streak/consecutive. */
        const uint32_t prev = om->bind[bi].as002_absent_ms;
        if (prev > UINT32_MAX - win_ms) {
            om->bind[bi].as002_absent_ms = UINT32_MAX;
        } else {
            om->bind[bi].as002_absent_ms = prev + win_ms;
        }
        om->diag.as002_sig_absent_advance++;

        if (om->bind[bi].as002_absent_ms < hold_ms) {
            continue;
        }

        const int64_t iv_end =
            window_end_mono_us > 0 ? window_end_mono_us
                                   : om->bind[bi].last_emit_mono_us;
        int64_t iv_start = iv_end - ((int64_t)win_ms * 1000LL);
        if (iv_start < om->bind[bi].start_mono_us) {
            iv_start = om->bind[bi].start_mono_us;
        }
        om->diag.as002_sig_end++;
        end_binding(
            om,
            (uint8_t)bi,
            EAR_OBS_END_SIGNATURE,
            iv_start,
            iv_end,
            has_signal_level_dbfs,
            signal_level_dbfs,
            false /* drain pending after this single end */
        );
    }
}

void ear_obs_manager_on_activity_periodic_hint(
    ear_obs_manager_t *om,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs
)
{
    if (om == NULL || om->cfg.emit_periodic == NULL) {
        return;
    }
    om->diag.periodic_hint++;
    for (uint8_t bi = 0u; bi < EAR_OBS_MGR_BINDING_CAP; bi++) {
        if (!om->bind[bi].in_use || !slot_start_accepted(om, om->bind[bi].ep)) {
            continue;
        }
        as001_maybe_deepen(om, bi, EAR_CLS_PHASE_PERIODIC);
        ear_obs_binding_view_t view;
        fill_binding_view(om, bi, &view);
        if (om->cfg.emit_periodic(
                om->cfg.emit_ctx,
                &view,
                interval_start_mono_us,
                interval_end_mono_us,
                has_signal_level_dbfs,
                signal_level_dbfs
            )) {
            om->bind[bi].last_emit_mono_us = interval_end_mono_us;
            if (om->cfg.registry != NULL) {
                sp_episode_registry_note_periodic_published(
                    om->cfg.registry, om->bind[bi].ep, interval_end_mono_us
                );
            }
        }
    }
}

void ear_obs_manager_force_end_all(
    ear_obs_manager_t *om,
    ear_obs_end_reason_t reason,
    int64_t now_mono_us
)
{
    if (om == NULL) {
        return;
    }
    char obs_ids[EAR_OBS_MGR_BINDING_CAP][SP_OBS_EPISODE_ID_LEN];
    uint8_t n_ids = 0u;
    for (uint8_t i = 0u; i < EAR_OBS_MGR_BINDING_CAP; i++) {
        if (!om->bind[i].in_use) {
            continue;
        }
        memcpy(
            obs_ids[n_ids],
            om->bind[i].observation_id,
            SP_OBS_EPISODE_ID_LEN
        );
        n_ids++;
    }
    for (uint8_t j = 0u; j < n_ids; j++) {
        const int bi = find_binding_by_observation_id(om, obs_ids[j]);
        if (bi < 0) {
            continue;
        }
        end_binding(
            om, (uint8_t)bi, reason, 0, now_mono_us, false, 0.0f, true
        );
    }
    drain_pending_to_open(om, now_mono_us);
}

bool ear_obs_manager_binding_end_hint_latched(
    const ear_obs_manager_t *om,
    uint64_t candidate_id
)
{
    const int bi = find_binding(om, candidate_id);
    return bi >= 0 && om->bind[bi].end_hint_latched;
}

void ear_obs_manager_on_engine_stop(ear_obs_manager_t *om, int64_t now_mono_us)
{
    if (om == NULL) {
        return;
    }

    /* Drain any EXIT/ENTER already queued from ce_stop. */
    process_event_queue(om, now_mono_us);

    for (uint8_t i = 0u; i < EAR_OBS_MGR_RSV_SLOTS; i++) {
        if (om->rsv[i].state == EAR_OBS_RSV_ST_HELD) {
            clear_rsv_slot(om, i);
            om->diag.reserve_released++;
        }
    }

    while (om->pending_count > 0u) {
        const uint8_t slot = om->pending_order[0];
        if (om->pending[slot].in_use && !om->pending[slot].cancelled) {
            om->pending[slot].cancelled = true;
        }
        pending_remove_at(om, 0u);
    }

    for (uint8_t bi = 0u; bi < EAR_OBS_MGR_BINDING_CAP; bi++) {
        if (om->bind[bi].in_use && om->bind[bi].ce_live) {
            om->bind[bi].ce_live = false;
            om->diag.exit_detach++;
            arm_detached_watchdog(om, bi, now_mono_us);
        }
    }
    om->cls_arm_valid = false;
    ear_cls_evidence_clear(&om->cls_arm);
}

void ear_obs_manager_forget_bindings(ear_obs_manager_t *om)
{
    if (om == NULL) {
        return;
    }
    for (uint8_t i = 0u; i < EAR_OBS_MGR_RSV_SLOTS; i++) {
        if (om->rsv[i].state == EAR_OBS_RSV_ST_HELD) {
            clear_rsv_slot(om, i);
            om->diag.reserve_released++;
        }
    }
    while (om->pending_count > 0u) {
        pending_remove_at(om, 0u);
    }
    for (uint8_t bi = 0u; bi < EAR_OBS_MGR_BINDING_CAP; bi++) {
        if (om->bind[bi].in_use) {
            note_binding_released(om, bi);
            memset(&om->bind[bi], 0, sizeof(om->bind[bi]));
        }
    }
    om->cls_arm_valid = false;
    ear_cls_evidence_clear(&om->cls_arm);
}

void ear_obs_manager_poll(ear_obs_manager_t *om, int64_t now_mono_us)
{
    if (om == NULL) {
        return;
    }

    process_event_queue(om, now_mono_us);

    /* Pending timeouts (copy order indices; timeout mutates queue). */
    uint8_t i = 0u;
    while (i < om->pending_count) {
        const uint8_t slot = om->pending_order[i];
        if (om->pending[slot].in_use && !om->pending[slot].cancelled
            && now_mono_us >= om->pending[slot].deadline_mono_us) {
            timeout_pending_at(om, i, now_mono_us);
            /* Queue compacted; re-scan from same index. */
            continue;
        }
        i++;
    }

    /*
     * Stage 3 detached-Obs watchdog: end only the expired binding.
     * Snapshot observation_ids so END cannot disturb iteration.
     */
    {
        const uint32_t timeout_ms = detached_end_timeout_ms(om);
        const int64_t timeout_us = (int64_t)timeout_ms * 1000LL;
        char obs_ids[EAR_OBS_MGR_BINDING_CAP][SP_OBS_EPISODE_ID_LEN];
        uint8_t n_ids = 0u;
        for (uint8_t bi = 0u; bi < EAR_OBS_MGR_BINDING_CAP; bi++) {
            if (!om->bind[bi].in_use || om->bind[bi].ce_live) {
                continue;
            }
            if (om->bind[bi].detached_since_mono_us <= 0) {
                continue;
            }
            if (binding_is_as002(om, bi)) {
                continue;
            }
            if (now_mono_us < om->bind[bi].detached_since_mono_us) {
                /* Non-monotonic clock: do not expire yet. */
                continue;
            }
            if ((now_mono_us - om->bind[bi].detached_since_mono_us) < timeout_us) {
                continue;
            }
            memcpy(
                obs_ids[n_ids],
                om->bind[bi].observation_id,
                SP_OBS_EPISODE_ID_LEN
            );
            n_ids++;
        }
        for (uint8_t j = 0u; j < n_ids; j++) {
            const int bi = find_binding_by_observation_id(om, obs_ids[j]);
            if (bi < 0 || om->bind[bi].ce_live || binding_is_as002(om, (uint8_t)bi)) {
                continue;
            }
            const int64_t detached_since = om->bind[bi].detached_since_mono_us;
            if (detached_since <= 0) {
                continue;
            }
            om->diag.detached_watchdog_end++;
            end_binding(
                om,
                (uint8_t)bi,
                EAR_OBS_END_EXPLICIT,
                detached_since,
                now_mono_us,
                false,
                0.0f,
                false
            );
        }
    }

    drain_pending_to_open(om, now_mono_us);

    /* Optional poll-driven PERIODIC (host tests); device uses activity hints. */
    if (om->cfg.enable_poll_periodic && om->cfg.emit_periodic != NULL) {
        const int64_t interval_us =
            (int64_t)om->cfg.observation_interval_ms * 1000LL;
        for (uint8_t bi = 0u; bi < EAR_OBS_MGR_BINDING_CAP; bi++) {
            if (!om->bind[bi].in_use || !slot_start_accepted(om, om->bind[bi].ep)) {
                continue;
            }
            if (now_mono_us - om->bind[bi].last_emit_mono_us < interval_us) {
                continue;
            }
            const int64_t iv_start = now_mono_us - interval_us;
            as001_maybe_deepen(om, bi, EAR_CLS_PHASE_PERIODIC);
            ear_obs_binding_view_t view;
            fill_binding_view(om, bi, &view);
            if (om->cfg.emit_periodic(
                    om->cfg.emit_ctx,
                    &view,
                    iv_start,
                    now_mono_us,
                    false,
                    0.0f
                )) {
                om->bind[bi].last_emit_mono_us = now_mono_us;
                if (om->cfg.registry != NULL) {
                    sp_episode_registry_note_periodic_published(
                        om->cfg.registry, om->bind[bi].ep, now_mono_us
                    );
                }
            }
        }
    }

    /* After queued events, watchdog/terminal END, FIFO drain, and PERIODIC. */
    retry_unaccepted_starts(om, now_mono_us);
}

void ear_obs_manager_arm_cls_evidence(
    ear_obs_manager_t *om,
    const ear_cls_evidence_t *evidence
)
{
    if (om == NULL) {
        return;
    }
    if (evidence == NULL) {
        om->cls_arm_valid = false;
        ear_cls_evidence_clear(&om->cls_arm);
        return;
    }
    om->cls_arm = *evidence;
    om->cls_arm_valid = true;
}

bool ear_obs_manager_feed_cls_evidence(
    ear_obs_manager_t *om,
    uint64_t candidate_id,
    const ear_cls_evidence_t *evidence,
    ear_cls_phase_t phase
)
{
    if (om == NULL || evidence == NULL) {
        return false;
    }
    const int bi = find_binding(om, candidate_id);
    if (bi < 0) {
        return false;
    }
    /* Approved AS-002 bindings own progressive siren assessment writes. */
    if (!binding_is_as002(om, (uint8_t)bi)) {
        return false;
    }
    return ear_cls_apply(
        &om->bind[bi].cls, evidence, phase, &om->bind[bi].cls_wire
    );
}

bool ear_obs_manager_get_cls_assessment(
    const ear_obs_manager_t *om,
    uint64_t candidate_id,
    ear_cls_assessment_wire_t *out_wire
)
{
    if (om == NULL) {
        return false;
    }
    const int bi = find_binding(om, candidate_id);
    if (bi < 0) {
        return false;
    }
    if (out_wire != NULL) {
        *out_wire = om->bind[bi].cls_wire;
    }
    return om->bind[bi].cls_wire.present;
}

