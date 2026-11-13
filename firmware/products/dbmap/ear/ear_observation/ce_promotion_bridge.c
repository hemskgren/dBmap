#include "ce_promotion_bridge.h"

#include <string.h>

/*
 * Device queue serialization (current firmware):
 * All production CE promotion callbacks and Bridge queue operations currently
 * execute on activity_task. EXIT may be enqueued during AE/CE polling; ENTER is
 * enqueued synchronously during ce_bridge_poll(). The queue is therefore
 * single-task serialized in the current firmware. Any future additional
 * callback producer or task must introduce appropriate synchronization and
 * revisit this assumption. Host tests may artificially saturate the queue;
 * that does not model device concurrency.
 */

void ce_bridge_init(ce_promotion_bridge_t *bridge)
{
    if (bridge == NULL) {
        return;
    }
    memset(bridge, 0, sizeof(*bridge));
}

void ce_bridge_bind_engine(ce_promotion_bridge_t *bridge, ce_engine_t *ce)
{
    if (bridge == NULL) {
        return;
    }
    bridge->ce = ce;
}

static bool queue_push(ce_promotion_bridge_t *bridge, uint64_t id, ce_promotion_event_t ev)
{
    if (bridge->q_count >= CE_BRIDGE_QUEUE_CAP) {
        bridge->diag.queue_overflows++;
        return false;
    }
    uint8_t idx = (uint8_t)((bridge->q_head + bridge->q_count) % CE_BRIDGE_QUEUE_CAP);
    bridge->queue[idx].candidate_id = id;
    bridge->queue[idx].ev = ev;
    bridge->q_count++;
    return true;
}

static bool queue_pop(ce_promotion_bridge_t *bridge, ce_bridge_event_t *out)
{
    if (bridge->q_count == 0u) {
        return false;
    }
    *out = bridge->queue[bridge->q_head];
    bridge->q_head = (uint8_t)((bridge->q_head + 1u) % CE_BRIDGE_QUEUE_CAP);
    bridge->q_count--;
    return true;
}

void ce_bridge_on_promotion(
    const ae_candidate_t *c,
    ce_promotion_event_t ev,
    void *ctx
)
{
    ce_promotion_bridge_t *bridge = (ce_promotion_bridge_t *)ctx;
    if (bridge == NULL || c == NULL) {
        return;
    }
    if (!queue_push(bridge, c->candidate_id, ev)) {
        return;
    }
    if (ev == CE_PROMOTION_ENTER) {
        bridge->diag.enter_queued++;
    } else if (ev == CE_PROMOTION_EXIT) {
        bridge->diag.exit_queued++;
    }
}

void ce_bridge_notify_obs_open(
    ce_promotion_bridge_t *bridge,
    const char *observation_id,
    sp_ep_handle_t handle
)
{
    if (bridge == NULL || observation_id == NULL || observation_id[0] == '\0') {
        return;
    }
    bridge->obs_context_valid = true;
    memset(bridge->obs_id, 0, sizeof(bridge->obs_id));
    {
        size_t n = strlen(observation_id);
        if (n >= sizeof(bridge->obs_id)) {
            n = sizeof(bridge->obs_id) - 1u;
        }
        memcpy(bridge->obs_id, observation_id, n);
    }
    bridge->obs_handle = handle;
}

void ce_bridge_notify_obs_closed(ce_promotion_bridge_t *bridge)
{
    if (bridge == NULL) {
        return;
    }
    if (bridge->assoc.bound) {
        bridge->diag.disassoc_on_obs_end++;
    }
    bridge->obs_context_valid = false;
    memset(bridge->obs_id, 0, sizeof(bridge->obs_id));
    memset(&bridge->obs_handle, 0, sizeof(bridge->obs_handle));
    memset(&bridge->assoc, 0, sizeof(bridge->assoc));
}

static bool handle_valid(
    const sp_episode_registry_t *registry,
    sp_ep_handle_t handle
)
{
    if (registry == NULL) {
        /* Host tests without registry: accept stored handle as-is. */
        return true;
    }
    return sp_episode_registry_handle_valid(registry, handle);
}

static void apply_enter(ce_promotion_bridge_t *bridge, uint64_t candidate_id)
{
    if (bridge->assoc.bound && bridge->assoc.candidate_id == candidate_id) {
        bridge->diag.enter_idempotent++;
        return;
    }

    /* Live association for a different candidate must not be silently replaced. */
    if (bridge->assoc.bound && bridge->assoc.candidate_id != candidate_id) {
        bridge->diag.assoc_reject_overwrite++;
        return;
    }

    if (!bridge->obs_context_valid || bridge->obs_id[0] == '\0') {
        bridge->diag.assoc_fail_no_obs++;
        return;
    }

    /* Generation check deferred to poll with registry; here use stored context. */
    memset(&bridge->assoc, 0, sizeof(bridge->assoc));
    bridge->assoc.bound = true;
    bridge->assoc.candidate_id = candidate_id;
    memcpy(
        bridge->assoc.observation_id,
        bridge->obs_id,
        sizeof(bridge->assoc.observation_id)
    );
    bridge->assoc.ep_handle = bridge->obs_handle;
    bridge->diag.assoc_success++;
}

static void apply_exit(ce_promotion_bridge_t *bridge, uint64_t candidate_id)
{
    if (!bridge->assoc.bound) {
        return;
    }
    if (bridge->assoc.candidate_id != candidate_id) {
        /* Stale EXIT for a different / replaced candidate. */
        return;
    }
    memset(&bridge->assoc, 0, sizeof(bridge->assoc));
    bridge->diag.disassoc_on_exit++;
}

static void drain_queue(
    ce_promotion_bridge_t *bridge,
    const sp_episode_registry_t *registry
)
{
    ce_bridge_event_t ev;
    while (queue_pop(bridge, &ev)) {
        if (ev.ev == CE_PROMOTION_ENTER) {
            if (bridge->obs_context_valid
                && !handle_valid(registry, bridge->obs_handle)) {
                bridge->diag.assoc_fail_stale_handle++;
                continue;
            }
            apply_enter(bridge, ev.candidate_id);
            if (bridge->assoc.bound
                && registry != NULL
                && !handle_valid(registry, bridge->assoc.ep_handle)) {
                memset(&bridge->assoc, 0, sizeof(bridge->assoc));
                bridge->diag.assoc_fail_stale_handle++;
                if (bridge->diag.assoc_success > 0u) {
                    bridge->diag.assoc_success--;
                }
            }
        } else if (ev.ev == CE_PROMOTION_EXIT) {
            apply_exit(bridge, ev.candidate_id);
        }
    }
}

bool ce_bridge_select_primary(const ce_engine_t *ce, uint64_t *out_candidate_id)
{
    if (ce == NULL || out_candidate_id == NULL || !ce->started) {
        return false;
    }

    bool have_promoted = false;
    bool have = false;
    uint64_t best_id = 0u;
    uint32_t best_evidence = 0u;
    float best_ema = 0.0f;

    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        const ae_candidate_t *c = &ce->slots[i];
        if (!c->in_use) {
            continue;
        }
        /*
         * Block while PROMOTED or while promotion_entered survives COASTING
         * (before rematch restores PROMOTED). Ranking among ACTIVE is unchanged.
         */
        if (c->state == AE_CAND_PROMOTED || c->promotion_entered) {
            have_promoted = true;
            break;
        }
    }
    if (have_promoted) {
        return false;
    }

    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        const ae_candidate_t *c = &ce->slots[i];
        if (!c->in_use || c->state != AE_CAND_ACTIVE) {
            continue;
        }
        if (c->quality_summary != AE_QUALITY_OK
            && c->quality_summary != AE_QUALITY_DEGRADED) {
            continue;
        }
        if (!have
            || c->evidence_count > best_evidence
            || (c->evidence_count == best_evidence && c->ema_rms_dbfs > best_ema)
            || (c->evidence_count == best_evidence && c->ema_rms_dbfs == best_ema
                && c->candidate_id < best_id)) {
            have = true;
            best_id = c->candidate_id;
            best_evidence = c->evidence_count;
            best_ema = c->ema_rms_dbfs;
        }
    }

    if (!have) {
        return false;
    }
    *out_candidate_id = best_id;
    return true;
}

/**
 * Locate the single live promoted CE lifecycle (PROMOTED or promotion_entered,
 * including temporary COASTING). If several somehow exist, prefer the lowest
 * candidate_id for determinism; one-primary normally admits at most one.
 */
static bool find_live_promoted_candidate(
    const ce_engine_t *ce,
    uint64_t *out_candidate_id
)
{
    if (ce == NULL || out_candidate_id == NULL || !ce->started) {
        return false;
    }

    bool have = false;
    uint64_t best_id = 0u;
    for (uint8_t i = 0; i < CE_MAX_CANDIDATES; i++) {
        const ae_candidate_t *c = &ce->slots[i];
        if (!c->in_use) {
            continue;
        }
        if (c->state != AE_CAND_PROMOTED && !c->promotion_entered) {
            continue;
        }
        if (!have || c->candidate_id < best_id) {
            have = true;
            best_id = c->candidate_id;
        }
    }
    if (!have) {
        return false;
    }
    *out_candidate_id = best_id;
    return true;
}

/**
 * Model A: bind an existing promoted lifecycle to the current Observation
 * without ce_promote / ENTER. Validates candidate identity and episode handle
 * generation before binding.
 */
static bool try_reassociate_promoted(
    ce_promotion_bridge_t *bridge,
    const sp_episode_registry_t *registry
)
{
    if (bridge == NULL || bridge->ce == NULL || bridge->assoc.bound) {
        return false;
    }
    if (!bridge->obs_context_valid || bridge->obs_id[0] == '\0') {
        return false;
    }
    if (registry != NULL && !handle_valid(registry, bridge->obs_handle)) {
        return false;
    }

    uint64_t cand_id = 0u;
    if (!find_live_promoted_candidate(bridge->ce, &cand_id)) {
        return false;
    }

    /* Re-read by identity: reject missing / cleared / no longer promoted. */
    ae_candidate_t live;
    if (!ce_find_candidate(bridge->ce, cand_id, &live)) {
        return false;
    }
    if (live.state != AE_CAND_PROMOTED && !live.promotion_entered) {
        return false;
    }
    if (live.candidate_id != cand_id) {
        return false;
    }

    memset(&bridge->assoc, 0, sizeof(bridge->assoc));
    bridge->assoc.bound = true;
    bridge->assoc.candidate_id = cand_id;
    memcpy(
        bridge->assoc.observation_id,
        bridge->obs_id,
        sizeof(bridge->assoc.observation_id)
    );
    bridge->assoc.ep_handle = bridge->obs_handle;
    bridge->diag.assoc_success++;

    if (registry != NULL
        && !handle_valid(registry, bridge->assoc.ep_handle)) {
        memset(&bridge->assoc, 0, sizeof(bridge->assoc));
        bridge->diag.assoc_fail_stale_handle++;
        if (bridge->diag.assoc_success > 0u) {
            bridge->diag.assoc_success--;
        }
        return false;
    }
    return true;
}

static void try_promote_primary(ce_promotion_bridge_t *bridge)
{
    if (bridge->ce == NULL || !bridge->obs_context_valid) {
        bridge->diag.promote_skipped++;
        return;
    }

    uint64_t primary = 0u;
    if (!ce_bridge_select_primary(bridge->ce, &primary)) {
        bridge->diag.promote_skipped++;
        return;
    }

    bridge->diag.promote_calls++;
    (void)ce_promote(bridge->ce, primary);
}

void ce_bridge_poll(
    ce_promotion_bridge_t *bridge,
    const sp_episode_registry_t *registry
)
{
    if (bridge == NULL) {
        return;
    }

    if (bridge->obs_context_valid && registry != NULL
        && !handle_valid(registry, bridge->obs_handle)) {
        ce_bridge_notify_obs_closed(bridge);
    }

    if (bridge->assoc.bound && registry != NULL
        && !handle_valid(registry, bridge->assoc.ep_handle)) {
        memset(&bridge->assoc, 0, sizeof(bridge->assoc));
        bridge->diag.assoc_fail_stale_handle++;
    }

    drain_queue(bridge, registry);

    /*
     * Association order (Model A):
     * 1) Rebind an existing promoted lifecycle to the current Observation
     *    (no ENTER / ce_promote).
     * 2) Else select primary ACTIVE → promote → ENTER → associate.
     */
    if (!bridge->assoc.bound) {
        if (!try_reassociate_promoted(bridge, registry)) {
            try_promote_primary(bridge);
            /* Promote may enqueue ENTER; drain again so association is current. */
            drain_queue(bridge, registry);
        }
    }
}

bool ce_bridge_is_bound(const ce_promotion_bridge_t *bridge)
{
    return bridge != NULL && bridge->assoc.bound;
}

uint64_t ce_bridge_bound_candidate_id(const ce_promotion_bridge_t *bridge)
{
    if (bridge == NULL || !bridge->assoc.bound) {
        return 0u;
    }
    return bridge->assoc.candidate_id;
}

const char *ce_bridge_bound_observation_id(const ce_promotion_bridge_t *bridge)
{
    if (bridge == NULL || !bridge->assoc.bound) {
        return "";
    }
    return bridge->assoc.observation_id;
}

void ce_bridge_get_diag(const ce_promotion_bridge_t *bridge, ce_bridge_diag_t *out)
{
    if (out == NULL) {
        return;
    }
    if (bridge == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = bridge->diag;
}

void ce_bridge_get_diag_snapshot(
    const ce_promotion_bridge_t *bridge,
    ce_bridge_diag_snapshot_t *out
)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (bridge == NULL) {
        return;
    }
    out->diag = bridge->diag;
    if (bridge->assoc.bound) {
        out->bound = true;
        out->candidate_id = bridge->assoc.candidate_id;
        memcpy(
            out->observation_id,
            bridge->assoc.observation_id,
            sizeof(out->observation_id)
        );
        out->observation_id[sizeof(out->observation_id) - 1u] = '\0';
    }
}
