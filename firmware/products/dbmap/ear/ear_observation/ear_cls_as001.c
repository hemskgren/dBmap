#include "ear_cls_as001.h"

#include <math.h>
#include <string.h>

/** Must match as_profile_as001().match_distance_max (taxonomy only). */
#define EAR_CLS_AS001_DISTANCE_SPAN 6.0f

static float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static float leaf_confidence_from_distance(float match_distance)
{
    /* Helicopter leaf: match-quality (distance 0 → high, at max → floor). */
    if (!isfinite(match_distance) || match_distance < 0.0f) {
        return 0.80f;
    }
    const float t =
        clampf(match_distance / EAR_CLS_AS001_DISTANCE_SPAN, 0.0f, 1.0f);
    return clampf(0.95f - 0.35f * t, 0.55f, 0.95f);
}

static float root_confidence_from_distance(float match_distance)
{
    /*
     * AIRCRAFT root: broader / at least as confident as the leaf
     * (ear_hyp_v2 allows independent per-node confidence).
     */
    const float leaf = leaf_confidence_from_distance(match_distance);
    return clampf(leaf + 0.05f, 0.60f, 0.95f);
}

static void set_aircraft_node(ear_cls_path_node_wire_t *node, float conf)
{
    memset(node, 0, sizeof(*node));
    strncpy(
        node->id, EAR_CLS_AS001_NODE_AIRCRAFT, sizeof(node->id) - 1u
    );
    node->confidence = conf;
}

static void set_helicopter_node(ear_cls_path_node_wire_t *node, float conf)
{
    memset(node, 0, sizeof(*node));
    strncpy(
        node->id, EAR_CLS_AS001_NODE_HELICOPTER, sizeof(node->id) - 1u
    );
    node->confidence = conf;
}

bool ear_cls_as001_apply(
    ear_cls_assessment_wire_t *inout,
    float match_distance,
    ear_cls_phase_t phase
)
{
    if (inout == NULL) {
        return false;
    }

    const float conf_root = root_confidence_from_distance(match_distance);
    const float conf_leaf = leaf_confidence_from_distance(match_distance);

    if (phase == EAR_CLS_PHASE_START || !inout->present) {
        memset(inout, 0, sizeof(*inout));
        set_aircraft_node(&inout->nodes[0], conf_root);
        inout->n_nodes = 1u;
        inout->deepest = EAR_CLS_NODE_NONE;
        inout->deepest_confidence = conf_root;
        inout->present = true;
        return true;
    }

    /* PERIODIC / END: deepen to leaf if still parent-only; refresh confidences. */
    if (inout->n_nodes == 0u) {
        set_aircraft_node(&inout->nodes[0], conf_root);
        inout->n_nodes = 1u;
    } else {
        set_aircraft_node(&inout->nodes[0], conf_root);
    }

    if (inout->n_nodes < 2u
        || strcmp(inout->nodes[1].id, EAR_CLS_AS001_NODE_HELICOPTER) != 0) {
        set_helicopter_node(&inout->nodes[1], conf_leaf);
        inout->n_nodes = 2u;
    } else {
        inout->nodes[1].confidence = conf_leaf;
    }

    inout->deepest = EAR_CLS_NODE_NONE;
    inout->deepest_confidence = conf_leaf;
    inout->present = true;
    return true;
}

bool ear_cls_as001_fill_assessment(
    ear_cls_assessment_wire_t *out,
    float match_distance
)
{
    /* Legacy name: START parent-only (lifecycle-correct default). */
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    return ear_cls_as001_apply(out, match_distance, EAR_CLS_PHASE_START);
}
