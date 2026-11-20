/**
 * Progressive ALARM taxonomy classification for open Observations.
 *
 * Taxonomy:
 *   ALARM
 *     └── SE_PUBLIC_ALERT
 *           ├── FARAN_OVER
 *           └── VMA
 *
 * Does not own Observation create/promote. Refinement updates assessment on an
 * existing episode only. Sibling correction is END-only with strong evidence.
 *
 * AS-001 helicopter assessment (AIRCRAFT > Helicopter) lives in ear_cls_as001
 * and is stamped at Observation bind — not through this ALARM module.
 *
 * MQTT wire format (Hub-accepted ear_hyp_v2):
 *   acoustic.assessment.schema = "ear_hyp_v2"
 *   acoustic.assessment.version = 1
 *   acoustic.assessment.path = [ {id, confidence}, ... ]  root→deepest
 *
 * Per-node confidence is independent of path depth. Hub also still accepts
 * legacy ear_hyp_v1; this module emits v2 only.
 *
 * Acoustic evidence source: ear_siren_se_public (shared SE public-alert siren)
 * arms/feeds this module on the device path. Assessment still appears only
 * when evidence is armed/fed — this module does not invent matcher results.
 */
#pragma once

#include "ear_cls_evidence.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Max taxonomy depth for ALARM branch (ALARM → SE → leaf). */
#define EAR_CLS_PATH_MAX_NODES 3u
/** Max chars for one taxonomy id including NUL. */
#define EAR_CLS_NODE_ID_MAX 32u

typedef enum {
    EAR_CLS_NODE_NONE = 0,
    EAR_CLS_NODE_ALARM = 1,
    EAR_CLS_NODE_SE_PUBLIC_ALERT = 2,
    EAR_CLS_NODE_FARAN_OVER = 3,
    EAR_CLS_NODE_VMA = 4,
} ear_cls_node_id_t;

typedef enum {
    EAR_CLS_PHASE_START = 0,
    EAR_CLS_PHASE_PERIODIC = 1,
    EAR_CLS_PHASE_END = 2,
} ear_cls_phase_t;

/**
 * Evidence thresholds (conservative; prefer broad over unsupported leaf).
 *
 * Promote/report ALARM when alarm_confidence >= EAR_CLS_ALARM_MIN_CONF.
 * Deepen to SE_PUBLIC_ALERT when se_confidence >= EAR_CLS_SE_MIN_CONF
 *   (and ALARM already supported).
 * Deepen to FARAN_OVER / VMA on START/PERIODIC only when leaf_confidence >=
 *   EAR_CLS_LEAF_PERIODIC_MIN_CONF and leaf_support_ms >=
 *   EAR_CLS_LEAF_PERIODIC_MIN_SUPPORT_MS (blocks premature leaf from brief
 *   broad-siren evidence).
 *
 * Sibling correction (FARAN_OVER ↔ VMA) is forbidden on PERIODIC.
 * At END, correction requires all of:
 *   - proposed leaf_confidence >= EAR_CLS_END_CORR_MIN_CONF
 *   - proposed leaf_support_ms >= EAR_CLS_END_CORR_MIN_SUPPORT_MS
 *   - proposed conf - current leaf conf >= EAR_CLS_END_CORR_MARGIN
 *   - proposed support_ms >= EAR_CLS_END_CORR_SUPPORT_RATIO * current support_ms
 */
#define EAR_CLS_ALARM_MIN_CONF 0.50f
#define EAR_CLS_SE_MIN_CONF 0.60f
#define EAR_CLS_LEAF_PERIODIC_MIN_CONF 0.75f
#define EAR_CLS_LEAF_PERIODIC_MIN_SUPPORT_MS 8000u
#define EAR_CLS_END_CORR_MIN_CONF 0.85f
#define EAR_CLS_END_CORR_MIN_SUPPORT_MS 20000u
#define EAR_CLS_END_CORR_MARGIN 0.20f
#define EAR_CLS_END_CORR_SUPPORT_RATIO 1.5f

/* ear_cls_evidence_t is defined in ear_cls_evidence.h (signature_engine). */

typedef struct {
    char id[EAR_CLS_NODE_ID_MAX];
    float confidence;
} ear_cls_path_node_wire_t;

typedef struct {
    bool present;
    uint8_t n_nodes;
    ear_cls_path_node_wire_t nodes[EAR_CLS_PATH_MAX_NODES];
    ear_cls_node_id_t deepest;
    float deepest_confidence;
} ear_cls_assessment_wire_t;

typedef struct {
    bool initialized;
    ear_cls_node_id_t deepest;
    float conf_alarm;
    float conf_se;
    float conf_leaf;
    ear_cls_node_id_t leaf; /* FARAN_OVER, VMA, or NONE */
    uint32_t leaf_support_ms;
    /** Contending sibling accumulators (for END correction only). */
    ear_cls_node_id_t contest_leaf;
    float contest_conf;
    uint32_t contest_support_ms;
    uint32_t refine_count;
    uint32_t correction_count;
} ear_cls_state_t;

void ear_cls_state_init(ear_cls_state_t *st);

void ear_cls_evidence_clear(ear_cls_evidence_t *ev);

const char *ear_cls_node_name(ear_cls_node_id_t node);

/**
 * Apply evidence for one report phase. Deepens conservatively.
 * Never opens Observations. Sibling swap only on EAR_CLS_PHASE_END when
 * correction thresholds are met.
 * Returns true when assessment is present after the update.
 */
bool ear_cls_apply(
    ear_cls_state_t *st,
    const ear_cls_evidence_t *ev,
    ear_cls_phase_t phase,
    ear_cls_assessment_wire_t *out_wire
);

/** Snapshot current assessment without mutating (out_wire may be NULL). */
bool ear_cls_snapshot(const ear_cls_state_t *st, ear_cls_assessment_wire_t *out_wire);

#ifdef __cplusplus
}
#endif
