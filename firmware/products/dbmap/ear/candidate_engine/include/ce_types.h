/**
 * Candidate Engine v1 — acoustic-signature tracking (not Observations).
 * Audio-signature-specific in this design (§0 principle 5).
 */
#pragma once

#include "ae_types.h"
#include "as_profile.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AE_CAND_NASCENT = 0,
    AE_CAND_ACTIVE = 1,
    AE_CAND_COASTING = 2,
    AE_CAND_PROMOTED = 3,
    AE_CAND_REJECTED = 4,
    AE_CAND_EXPIRED = 5,
} ae_cand_state_t;

typedef struct {
    uint64_t candidate_id;
    ae_cand_state_t state;
    int64_t created_mono_us;
    int64_t last_evidence_mono_us;
    int64_t last_update_mono_us;
    uint32_t evidence_count;
    uint32_t coast_windows;      /* Transport/quality gap steps only. */
    uint32_t valid_miss_windows; /* Usable windows since the last hard match. */
    float confidence;
    uint8_t quality_summary;
    uint32_t last_window_seq;
    float ema_rms_dbfs;
    /** Absolute band EMA (dBFS); identity distance uses shape relative to RMS. */
    float band_ema[AE_BAND_COUNT];
    /**
     * Spectral EMA — only valid when spectral_ema_valid. Missing/invalid
     * spectral input must not be treated as zero evidence.
     */
    float ema_dominant_hz;
    float ema_flatness;
    float ema_centroid_hz;
    bool spectral_ema_valid;
    /**
     * Pair ΔRMS EMA (mic0−mic1 dB) — stereo asymmetry cue only, not DoA/TDOA.
     * Valid only when pair_ema_valid.
     */
    float ema_delta_rms_db;
    bool pair_ema_valid;
    uint8_t mic_mask_seen;
    bool in_use;
    /**
     * Set when CE_PROMOTION_ENTER is issued; cleared when CE_PROMOTION_EXIT is
     * issued. Survives PROMOTED → COASTING so expiry still notifies the bridge.
     */
    bool promotion_entered;
    /**
     * Promotion association generation assigned at CE_PROMOTION_ENTER from
     * ce_engine_t.next_promotion_gen (engine-monotonic, never zero after the
     * first ENTER). Preserved on the EXIT callback payload — emit_exit invokes
     * the callback before clearing promotion_entered and does not clear this
     * field until slot_clear. Rematch without a new ENTER does not bump it.
     * Consumers must match this on EXIT so a delayed EXIT cannot affect a
     * newer association that reused the same candidate_id after slot clear.
     * Zero means "unset" (never ENTER'd, or synthetic payload without gen).
     */
    uint32_t promotion_gen;
    /**
     * Times a sequence discontinuity or GAP-quality signature was applied to
     * this slot. Not a fabricated timestamp; the signature's own
     * window_end_mono_us remains the evidence clock.
     */
    uint16_t gap_events;
} ae_candidate_t;

/**
 * Classification annotation associated with a Candidate (workflow/classifier).
 * Not part of Stage 1 identity. Does not imply promotion or Observation.
 */
typedef struct {
    uint64_t candidate_id;
    bool valid;
    bool matched;
    uint16_t profile_id;
    as_acoustic_class_t acoustic_class;
    float distance;
    /**
     * Candidate gap_events at the time this annotation was written.
     * After a later GAP, gap_events advances and this annotation is stale
     * for product-relevance gating until a fresh workflow classification.
     */
    uint16_t gap_events_at_match;
} ce_classification_t;

typedef enum {
    CE_PROMOTION_ENTER = 0,
    CE_PROMOTION_EXIT = 1,
} ce_promotion_event_t;

typedef void (*ce_promotion_cb_t)(
    const ae_candidate_t *c,
    ce_promotion_event_t ev,
    void *ctx
);

#ifdef __cplusplus
}
#endif
