/**
 * EAR Observation Manager — promotion-gated Observation lifecycle (product).
 *
 * Sole owner of EAR Observation create/bind/update/end. Platform registry is
 * used only as generic slot infrastructure.
 *
 * Single-task contract (device): all public APIs and CE promotion callbacks
 * must run on the same task as ear_obs activity_task (same model as the former
 * ce_promotion_bridge). Host tests are single-threaded.
 */
#pragma once

#include "ae_engine.h"
#include "ae_types.h"
#include "ce_engine.h"
#include "ce_types.h"
#include "ear_cls_progressive.h"
#include "sp_episode_registry.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bounded owned signature evidence per Observation binding / promote snapshot.
 *
 * ENTER may seed up to this many by-value copies from AE history (unchanged).
 * Post-promotion live updates append candidate-associated signatures into the
 * same ring. Semantics: newest at ``sigs[0]`` (FIFO toward higher indices);
 * when full, the oldest entry is dropped on insert. Cap is compile-time and
 * must stay aligned with Firmware Map capacities (CE Stage 2).
 */
#ifndef EAR_OBS_PROMOTE_SIG_CAP
#define EAR_OBS_PROMOTE_SIG_CAP 3u
#endif

#define EAR_OBS_MGR_RSV_SLOTS 2u
#define EAR_OBS_MGR_PENDING_CAP 2u
#define EAR_OBS_MGR_BINDING_CAP 2u
#define EAR_OBS_MGR_EVENT_Q 8u

#define EAR_OBS_PENDING_TIMEOUT_MS_DEFAULT 30000u

typedef enum {
    EAR_OBS_RSV_OK = 0,
    EAR_OBS_RSV_FULL = 1,
    EAR_OBS_RSV_INVALID = 2,
} ear_obs_rsv_result_t;

typedef enum {
    EAR_OBS_RSV_ST_NONE = 0,
    EAR_OBS_RSV_ST_HELD = 1,
    EAR_OBS_RSV_ST_PENDING = 2,
    EAR_OBS_RSV_ST_BOUND = 3,
} ear_obs_rsv_state_t;

typedef struct {
    uint32_t magic;
    uint16_t index;
    uint16_t generation;
} ear_obs_rsv_handle_t;

typedef struct {
    uint64_t candidate_id;
    int64_t promote_mono_us;
    int64_t last_evidence_mono_us;
    uint32_t last_window_seq;
    uint32_t evidence_count;
    float ema_rms_dbfs;
    float band_ema[AE_BAND_COUNT];
    uint8_t quality_summary;
    uint8_t state_at_promote;
    uint16_t gap_events;
    /** CE promotion_gen captured at ENTER (0 if unknown / non-CE path). */
    uint32_t promotion_gen;
    /**
     * P2 product-relevance stamp (deny-by-default).
     *
     * Invariant: pending => product_relevance_approved.
     * Carried from Promotion Policy via reservation ticket at ENTER — not
     * reconstructed from live CE classification. Drain validates the stamp
     * only; it never invents approval.
     */
    bool product_relevance_approved;
    uint16_t relevance_profile_id;
    float relevance_match_distance;
    /** Non-zero ticket from reservation stamp (Policy / explicit test stamp). */
    uint32_t relevance_ticket;
    uint8_t sig_count;
    ae_signature_t sigs[EAR_OBS_PROMOTE_SIG_CAP];
} ear_obs_promote_snapshot_t;

typedef enum {
    EAR_OBS_END_HINT_ACTIVITY = 1,
    EAR_OBS_END_OTA = 2,
    EAR_OBS_END_EXPLICIT = 3,
    /** AS002 usable-window tonal support absent longer than configured hold. */
    EAR_OBS_END_SIGNATURE = 4,
} ear_obs_end_reason_t;

/**
 * Activity END fan-in kind. Distinguishes ambient RMS END from invalid-hold
 * safety so AS002 can ignore the former but not the latter.
 */
typedef enum {
    /**
     * Natural / relative / secondary-ambient RMS END.
     * Ends non-AS002 bindings; leaves AS002 signature-backed Obs open.
     */
    EAR_OBS_ACT_END_AMBIENT = 1,
    /**
     * Activity max-invalid-hold safety (CONFIG_DBMAP_EAR_OBS_MAX_INVALID_HOLD_MS).
     * Ends every open binding, including AS002.
     */
    EAR_OBS_ACT_END_INVALID_HOLD = 2,
} ear_obs_activity_end_kind_t;

/**
 * Default AS002 signature-absence hold (ms). Conservative within the reviewed
 * 45–60 s range so short-pulse (~14 s) gaps do not premature-END. Overridden
 * by ear_obs_manager_config_t.as002_signature_absence_hold_ms / Kconfig.
 */
#ifndef EAR_OBS_AS002_SIGNATURE_ABSENCE_HOLD_MS_DEFAULT
#define EAR_OBS_AS002_SIGNATURE_ABSENCE_HOLD_MS_DEFAULT 60000u
#endif

/**
 * Default OM detached-Observation watchdog (ms). Stage 3: after a matching CE
 * EXIT clears ce_live on a normal (non-AS002) binding, if the Observation is
 * still open when this monotonic hold elapses, OM ends that binding alone with
 * EAR_OBS_END_EXPLICIT. Chosen to match the AS002 absence-hold scale (60 s) and
 * sit above pending_timeout (30 s) / max_invalid_hold (30 s) so brief re-promote
 * windows are not cut short. Overridden by
 * ear_obs_manager_config_t.detached_end_timeout_ms / Kconfig.
 */
#ifndef EAR_OBS_DETACHED_END_TIMEOUT_MS_DEFAULT
#define EAR_OBS_DETACHED_END_TIMEOUT_MS_DEFAULT 60000u
#endif

typedef struct {
    uint64_t candidate_id;
    sp_ep_handle_t ep;
    char observation_id[SP_OBS_EPISODE_ID_LEN];
    bool ce_live;
    int64_t start_mono_us;
    /**
     * ear_hyp_v2 assessment for this open episode (AS-002 progressive ALARM
     * and/or AS-001 AIRCRAFT > Helicopter). present=false when none yet.
     * Never implies a new Observation — same observation_id for the episode.
     */
    ear_cls_assessment_wire_t assessment;
} ear_obs_binding_view_t;

typedef bool (*ear_obs_emit_start_fn)(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    const ear_obs_promote_snapshot_t *snap,
    int64_t now_mono_us
);

typedef bool (*ear_obs_emit_periodic_fn)(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs
);

typedef bool (*ear_obs_emit_end_fn)(
    void *ctx,
    const ear_obs_binding_view_t *binding,
    ear_obs_end_reason_t reason,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs
);

typedef struct {
    sp_episode_registry_t *registry;
    ce_engine_t *ce;
    ae_engine_t *ae; /* optional; for history copy into snapshots */
    sp_ep_uuid_fn uuid_fn;
    uint32_t observation_interval_ms;
    uint32_t pending_timeout_ms;
    uint8_t producer_tag;
    ear_obs_emit_start_fn emit_start;
    ear_obs_emit_periodic_fn emit_periodic;
    ear_obs_emit_end_fn emit_end;
    void *emit_ctx;
    /**
     * When true, poll() emits PERIODIC on observation_interval_ms.
     * Device path sets false and drives PERIODIC via
     * ear_obs_manager_on_activity_periodic_hint (RMS cadence + L0 windows).
     */
    bool enable_poll_periodic;
    /**
     * AS002: usable non-tonal windows must accumulate this many ms before
     * signature END. 0 → EAR_OBS_AS002_SIGNATURE_ABSENCE_HOLD_MS_DEFAULT.
     */
    uint32_t as002_signature_absence_hold_ms;
    /**
     * Stage 3 detached-Obs watchdog. After matching CE EXIT on a non-AS002
     * open binding, end that binding if still open && !ce_live when this many
     * ms of monotonic time elapse. 0 → EAR_OBS_DETACHED_END_TIMEOUT_MS_DEFAULT.
     * AS002 is excluded (signature-absence END remains authoritative).
     * PERIODIC hints must not reset the timer.
     */
    uint32_t detached_end_timeout_ms;
} ear_obs_manager_config_t;

typedef struct {
    uint32_t reserve_ok;
    uint32_t reserve_full;
    uint32_t reserve_released;
    uint32_t promote_commit_bound;
    uint32_t promote_commit_pending;
    uint32_t promote_commit_fail;
    uint32_t enter_dup;
    uint32_t enter_no_rsv;
    uint32_t pending_enqueue;
    uint32_t pending_dequeue;
    uint32_t drain_attempts;
    uint32_t drain_alloc_fail;
    uint32_t pending_timeout;
    uint32_t pending_cancel_exit;
    /** Commit/enqueue/drain refused: snapshot lacked product-relevance stamp. */
    uint32_t relevance_reject;
    /** CE unadmit requested after failed Observation admission. */
    uint32_t promote_unadmit;
    uint32_t exit_detach;
    /**
     * EXIT ignored: missing/zero/mismatched promotion_gen against a gen-tagged
     * binding (no candidate_id-only detach fallback).
     */
    uint32_t exit_stale;
    uint32_t end_hint;
    uint32_t end_hint_latched;
    uint32_t end_hint_consumed;
    uint32_t end_done;
    uint32_t evidence_accepted;
    uint32_t evidence_dropped;
    /**
     * Signature copy offered but not admitted (GAP/INVALID, duplicate or
     * out-of-order window_seq, or provenance mismatch vs candidate update).
     * EMA fields from cand_copy may still have been applied.
     */
    uint32_t evidence_sig_rejected;
    uint32_t periodic_hint;
    /** Promotion callback arrived while EAR_OBS_MGR_EVENT_Q was full. */
    uint32_t evq_dropped;
    /** AS002 signature-absence END emissions. */
    uint32_t as002_sig_end;
    /** AS002 loss timer resets on current-window tonal support. */
    uint32_t as002_sig_support_reset;
    /** AS002 loss timer advances on usable non-tonal windows. */
    uint32_t as002_sig_absent_advance;
    /** AS002 loss timer paused (INVALID/unusable). */
    uint32_t as002_sig_absent_pause;
    /** Ambient activity END skipped an AS002 binding. */
    uint32_t activity_end_as002_skipped;
    /** Detached watchdog armed on matching EXIT (non-AS002). */
    uint32_t detached_watchdog_armed;
    /** Detached watchdog cancelled (ce_live restored / re-attach). */
    uint32_t detached_watchdog_cancelled;
    /** Observation ENDs emitted by detached watchdog expiry. */
    uint32_t detached_watchdog_end;
    /** ENTER re-attached ce_live on an open detached binding (same candidate). */
    uint32_t enter_reattach;
} ear_obs_manager_diag_t;

typedef struct ear_obs_manager ear_obs_manager_t;

void ear_obs_manager_init(ear_obs_manager_t *om, const ear_obs_manager_config_t *cfg);

ear_obs_rsv_result_t ear_obs_manager_reserve(
    ear_obs_manager_t *om,
    ear_obs_rsv_handle_t *out
);

bool ear_obs_manager_bind_reservation_candidate(
    ear_obs_manager_t *om,
    ear_obs_rsv_handle_t h,
    uint64_t candidate_id
);

/**
 * Attach Policy (or explicit test) product-relevance approval to a HELD
 * reservation. Required before ENTER can create an Observation. Bound to the
 * reservation's expected candidate_id; consumed at commit.
 */
bool ear_obs_manager_stamp_reservation_relevance(
    ear_obs_manager_t *om,
    ear_obs_rsv_handle_t h,
    uint64_t candidate_id,
    uint16_t profile_id,
    float match_distance
);

bool ear_obs_manager_commit_reservation(
    ear_obs_manager_t *om,
    ear_obs_rsv_handle_t h,
    uint64_t candidate_id,
    const ear_obs_promote_snapshot_t *snapshot
);

void ear_obs_manager_release_reservation(
    ear_obs_manager_t *om,
    ear_obs_rsv_handle_t h
);

/** CE promotion callback — enqueue only. Register via ce_set_promotion_cb. */
void ear_obs_manager_on_promotion(
    const ae_candidate_t *c,
    ce_promotion_event_t ev,
    void *ctx
);

/**
 * Refresh a ce_live binding from CE state and optional signature evidence.
 *
 * ``cand_copy`` updates EMA / seq / gap fields when present.
 * ``sig_copy`` is admitted only when:
 *   - quality is OK or DEGRADED (never INVALID or GAP),
 *   - window_seq > last owned signature seq for this binding,
 *   - when cand_copy is also provided, sig.window_seq == cand.last_window_seq
 *     (exact association with that candidate update — not AE "latest"),
 *   - promotion_gen matches the live binding when both sides are non-zero.
 *
 * Owned copies only: the signature is memcpy'd into the binding ring; AE
 * history overwrite cannot mutate admitted evidence. Ring: newest at [0],
 * capacity EAR_OBS_PROMOTE_SIG_CAP, drop oldest when full.
 */
void ear_obs_manager_on_candidate_evidence(
    ear_obs_manager_t *om,
    uint64_t candidate_id,
    const ae_signature_t *sig_copy,
    const ae_candidate_t *cand_copy
);

/**
 * For each open ce_live binding, feed CE candidate EMA plus the last
 * candidate-associated signature (``ce_get_last_evidence_signature``).
 * Does not call AE history APIs — preserves multi-candidate provenance.
 */
void ear_obs_manager_feed_from_ce(ear_obs_manager_t *om, ce_engine_t *ce);

/**
 * RMS activity END hint.
 *
 * EAR_OBS_ACT_END_AMBIENT: ends open bindings except AS002 (signature-backed
 * Obs ignore ambient RMS END).
 * EAR_OBS_ACT_END_INVALID_HOLD: ends every open binding, including AS002
 * (preserves max-invalid-hold safety).
 *
 * Does not wait for CE EXIT. EXIT alone never ENDs. Pending drain runs after
 * targeted ends. Repeated hints are no-ops once targeted bindings are gone.
 */
void ear_obs_manager_on_activity_end_hint(
    ear_obs_manager_t *om,
    ear_obs_activity_end_kind_t kind,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs
);

/**
 * Feed one siren current-window outcome into open AS002 bindings.
 *
 * usable + tonal_vote → reset absence hold (global siren timbre support; not
 * source-specific identity).
 * usable + !tonal_vote → advance absence hold by window_ms; END that binding
 * alone when hold expires.
 * !usable → pause hold (unknown evidence; never counts as support).
 *
 * Does not use consecutive/streak as current support.
 */
void ear_obs_manager_on_as002_siren_window(
    ear_obs_manager_t *om,
    bool usable,
    bool tonal_vote,
    uint32_t window_ms,
    int64_t window_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs
);

/**
 * RMS activity PERIODIC hint: publish PERIODIC for each open Observation
 * using the detector interval (preserves L0 window association on device).
 */
void ear_obs_manager_on_activity_periodic_hint(
    ear_obs_manager_t *om,
    int64_t interval_start_mono_us,
    int64_t interval_end_mono_us,
    bool has_signal_level_dbfs,
    float signal_level_dbfs
);

/**
 * Arm progressive evidence applied once at the next Observation bind
 * (START phase), before emit_start. Does not promote or create episodes.
 * Cleared after consumption or on engine stop / forget_bindings.
 */
void ear_obs_manager_arm_cls_evidence(
    ear_obs_manager_t *om,
    const ear_cls_evidence_t *evidence
);

/**
 * Feed progressive ALARM-taxonomy evidence into an open AS-002 Observation
 * without promoting again. Refinement updates assessment only; observation_id
 * is unchanged. phase selects START / PERIODIC / END correction rules.
 * Returns false if the candidate has no open binding or that binding is not
 * an approved AS-002 Observation.
 */
bool ear_obs_manager_feed_cls_evidence(
    ear_obs_manager_t *om,
    uint64_t candidate_id,
    const ear_cls_evidence_t *evidence,
    ear_cls_phase_t phase
);

/**
 * Copy current progressive assessment for an open binding into out_wire.
 * Returns false when unbound or assessment not present.
 */
bool ear_obs_manager_get_cls_assessment(
    const ear_obs_manager_t *om,
    uint64_t candidate_id,
    ear_cls_assessment_wire_t *out_wire
);

void ear_obs_manager_force_end_all(
    ear_obs_manager_t *om,
    ear_obs_end_reason_t reason,
    int64_t now_mono_us
);

/**
 * Drain ENTER/EXIT, pending timeouts, detached-Obs watchdog expiry, FIFO
 * drain, and optional PERIODIC ticks. ``now_mono_us`` must be monotonic
 * (device: esp_timer; host tests: synthetic advancing clock).
 */
void ear_obs_manager_poll(ear_obs_manager_t *om, int64_t now_mono_us);

/**
 * CE engine stop / teardown: release HELD and PENDING, detach ce_live.
 * Does not END open Observations (caller may force_end_all separately).
 */
void ear_obs_manager_on_engine_stop(ear_obs_manager_t *om, int64_t now_mono_us);

/**
 * Clear binding table without registry release (OTA path owns publish+release).
 * Also cancels PENDING and releases HELD.
 */
void ear_obs_manager_forget_bindings(ear_obs_manager_t *om);

void ear_obs_manager_fill_snapshot_from_candidate(
    ear_obs_manager_t *om,
    const ae_candidate_t *c,
    int64_t now_mono_us,
    ear_obs_promote_snapshot_t *out
);

uint8_t ear_obs_manager_n_open(const ear_obs_manager_t *om);
uint8_t ear_obs_manager_n_pending(const ear_obs_manager_t *om);
uint8_t ear_obs_manager_n_held(const ear_obs_manager_t *om);

/**
 * True when an open or pending Observation already carries the given
 * product-relevance profile stamp (e.g. AS-001 or AS-002). Used by Promotion
 * Policy to prevent a second concurrent Observation for the same recognized
 * acoustic signature while still allowing a different profile on the same EAR.
 * profile_id AS_PROFILE_ID_NONE always returns false.
 */
bool ear_obs_manager_has_signature_observation(
    const ear_obs_manager_t *om,
    uint16_t relevance_profile_id
);

void ear_obs_manager_get_diag(const ear_obs_manager_t *om, ear_obs_manager_diag_t *out);

bool ear_obs_manager_binding_ce_live(const ear_obs_manager_t *om, uint64_t candidate_id);

/**
 * Legacy latch query. Always false under immediate-END contract (P1);
 * retained so existing call sites compile. Prefer n_open / emit counts.
 */
bool ear_obs_manager_binding_end_hint_latched(
    const ear_obs_manager_t *om,
    uint64_t candidate_id
);

/* Opaque size for stack/static allocation in tests and firmware. */
struct ear_obs_manager {
    ear_obs_manager_config_t cfg;
    ear_obs_manager_diag_t diag;

    struct {
        ear_obs_rsv_state_t state;
        uint16_t generation;
        uint64_t expected_candidate_id;
        bool has_expected_id;
        ear_obs_promote_snapshot_t snap;
        bool has_snap;
        int64_t held_mono_us;
        bool relevance_approved;
        uint16_t relevance_profile_id;
        float relevance_match_distance;
        uint32_t relevance_ticket;
    } rsv[EAR_OBS_MGR_RSV_SLOTS];

    uint32_t next_relevance_ticket;

    struct {
        bool in_use;
        bool cancelled;
        uint64_t candidate_id;
        int64_t entered_mono_us;
        int64_t deadline_mono_us;
        ear_obs_promote_snapshot_t snap;
        ear_obs_rsv_handle_t rsv_h;
    } pending[EAR_OBS_MGR_PENDING_CAP];
    uint8_t pending_order[EAR_OBS_MGR_PENDING_CAP];
    uint8_t pending_count;

    struct {
        bool in_use;
        bool ce_live;
        /* Legacy latch fields unused under immediate-END; kept for layout. */
        bool end_hint_latched;
        uint64_t candidate_id;
        /** Matches ae_candidate_t.promotion_gen for the live CE association. */
        uint32_t ce_promotion_gen;
        sp_ep_handle_t ep;
        char observation_id[SP_OBS_EPISODE_ID_LEN];
        int64_t start_mono_us;
        int64_t last_emit_mono_us;
        int64_t end_hint_interval_start_mono_us;
        int64_t end_hint_interval_end_mono_us;
        bool end_hint_has_level;
        float end_hint_level_dbfs;
        ear_obs_promote_snapshot_t snap;
        uint32_t evidence_updates;
        /**
         * Highest window_seq among owned signature copies in snap.sigs[].
         * Used to reject duplicate / out-of-order live signature inserts.
         */
        uint32_t last_owned_sig_window_seq;
        /**
         * Stage 3: monotonic us when matching CE EXIT cleared ce_live (0 = not
         * armed). Cleared on re-attach / new bind / END. Not touched by PERIODIC.
         */
        int64_t detached_since_mono_us;
        /** Progressive ALARM taxonomy state (refinement; not a promote gate). */
        ear_cls_state_t cls;
        ear_cls_assessment_wire_t cls_wire;
        /**
         * AS002 signature-absence tracking (global siren current-window votes).
         * Accumulates only on usable non-tonal windows; paused on unusable.
         */
        bool as002_sig_track;
        uint32_t as002_absent_ms;
    } bind[EAR_OBS_MGR_BINDING_CAP];

    struct {
        uint64_t candidate_id;
        ce_promotion_event_t ev;
        ae_candidate_t cand_copy;
        bool have_cand;
    } evq[EAR_OBS_MGR_EVENT_Q];
    uint8_t evq_head;
    uint8_t evq_count;

    /** One-shot evidence for the next allocate_and_bind (START). */
    bool cls_arm_valid;
    ear_cls_evidence_t cls_arm;
};

#ifdef __cplusplus
}
#endif
