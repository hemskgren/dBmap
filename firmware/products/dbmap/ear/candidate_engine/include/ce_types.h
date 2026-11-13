/**
 * Candidate Engine v1 — acoustic-signature tracking (not Observations).
 * Audio-signature-specific in this design (§0 principle 5).
 */
#pragma once

#include "ae_types.h"

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
    uint32_t coast_windows;
    float confidence;
    uint8_t quality_summary;
    uint32_t last_window_seq;
    float ema_rms_dbfs;
    float band_ema[AE_BAND_COUNT];
    uint8_t mic_mask_seen;
    bool in_use;
    /**
     * Set when CE_PROMOTION_ENTER is issued; cleared when CE_PROMOTION_EXIT is
     * issued. Survives PROMOTED → COASTING so expiry still notifies the bridge.
     */
    bool promotion_entered;
} ae_candidate_t;

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
