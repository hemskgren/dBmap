/**
 * AS-001 helicopter assessment (ear_hyp_v2) for promoted Observations.
 *
 * V1 taxonomy path (root → leaf):
 *   AIRCRAFT
 *     └── Helicopter
 *
 * Lifecycle (aligned with AS-002 progressive deepen, not ALARM semantics):
 *   START    → path = [AIRCRAFT]
 *   PERIODIC → deepen to [AIRCRAFT, Helicopter] (idempotent)
 *   END      → deepen if still parent-only (short episodes without PERIODIC)
 *
 * Reuses ear_cls_assessment_wire_t / observation_payload ear_hyp_v2 serializer.
 * Does not use progressive ALARM / siren evidence (ear_cls_progressive).
 */
#pragma once

#include "ear_cls_progressive.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Root node id for AS-001 V1 assessment.path. */
#define EAR_CLS_AS001_NODE_AIRCRAFT "AIRCRAFT"
/** Leaf node id for AS-001 V1 assessment.path. */
#define EAR_CLS_AS001_NODE_HELICOPTER "Helicopter"

/**
 * Apply AS-001 assessment for a report phase.
 *
 * @param inout            Assessment wire (required); mutated in place.
 * @param match_distance   Promote match distance (lower is better).
 * @param phase            START → parent only; PERIODIC/END → deepen leaf.
 * @return true when inout->present after the update.
 */
bool ear_cls_as001_apply(
    ear_cls_assessment_wire_t *inout,
    float match_distance,
    ear_cls_phase_t phase
);

/**
 * Convenience: START parent-only fill (clears then applies START).
 * Prefer ear_cls_as001_apply for lifecycle.
 */
bool ear_cls_as001_fill_assessment(
    ear_cls_assessment_wire_t *out,
    float match_distance
);

#ifdef __cplusplus
}
#endif
