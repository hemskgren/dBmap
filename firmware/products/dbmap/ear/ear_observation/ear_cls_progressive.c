#include "ear_cls_progressive.h"

#include <string.h>

void ear_cls_state_init(ear_cls_state_t *st)
{
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof(*st));
    st->initialized = true;
}

void ear_cls_evidence_clear(ear_cls_evidence_t *ev)
{
    if (ev == NULL) {
        return;
    }
    memset(ev, 0, sizeof(*ev));
}

const char *ear_cls_node_name(ear_cls_node_id_t node)
{
    switch (node) {
    case EAR_CLS_NODE_ALARM:
        return "ALARM";
    case EAR_CLS_NODE_SE_PUBLIC_ALERT:
        return "SE_PUBLIC_ALERT";
    case EAR_CLS_NODE_FARAN_OVER:
        return "FARAN_OVER";
    case EAR_CLS_NODE_VMA:
        return "VMA";
    case EAR_CLS_NODE_NONE:
    default:
        return NULL;
    }
}

static bool is_leaf(ear_cls_node_id_t n)
{
    return n == EAR_CLS_NODE_FARAN_OVER || n == EAR_CLS_NODE_VMA;
}

static float clamp01(float v)
{
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

static float node_confidence(const ear_cls_state_t *st, ear_cls_node_id_t node)
{
    switch (node) {
    case EAR_CLS_NODE_ALARM:
        return st->conf_alarm;
    case EAR_CLS_NODE_SE_PUBLIC_ALERT:
        return st->conf_se;
    case EAR_CLS_NODE_FARAN_OVER:
    case EAR_CLS_NODE_VMA:
        return st->conf_leaf;
    case EAR_CLS_NODE_NONE:
    default:
        return 0.0f;
    }
}

static bool build_wire(const ear_cls_state_t *st, ear_cls_assessment_wire_t *out)
{
    if (out == NULL) {
        return st != NULL && st->deepest != EAR_CLS_NODE_NONE;
    }
    memset(out, 0, sizeof(*out));
    if (st == NULL || st->deepest == EAR_CLS_NODE_NONE) {
        return false;
    }

    const ear_cls_node_id_t chain[EAR_CLS_PATH_MAX_NODES] = {
        EAR_CLS_NODE_ALARM,
        EAR_CLS_NODE_SE_PUBLIC_ALERT,
        st->leaf != EAR_CLS_NODE_NONE ? st->leaf : EAR_CLS_NODE_NONE,
    };
    const uint8_t n_chain = (st->deepest == EAR_CLS_NODE_ALARM)             ? 1u
        : (st->deepest == EAR_CLS_NODE_SE_PUBLIC_ALERT)                     ? 2u
        : is_leaf(st->deepest)                                              ? 3u
                                                                            : 0u;
    for (uint8_t i = 0u; i < n_chain; i++) {
        const char *name = ear_cls_node_name(chain[i]);
        if (name == NULL) {
            break;
        }
        const size_t nlen = strlen(name);
        if (nlen + 1u > EAR_CLS_NODE_ID_MAX) {
            return false;
        }
        memcpy(out->nodes[out->n_nodes].id, name, nlen + 1u);
        out->nodes[out->n_nodes].confidence = clamp01(node_confidence(st, chain[i]));
        out->n_nodes++;
    }
    if (out->n_nodes == 0u) {
        return false;
    }

    out->present = true;
    out->deepest = st->deepest;
    out->deepest_confidence = out->nodes[out->n_nodes - 1u].confidence;
    return true;
}

static void note_contest(
    ear_cls_state_t *st,
    ear_cls_node_id_t leaf,
    float conf,
    uint32_t support_ms
)
{
    if (!is_leaf(leaf) || st->leaf == EAR_CLS_NODE_NONE || leaf == st->leaf) {
        return;
    }
    if (st->contest_leaf == leaf) {
        if (conf > st->contest_conf) {
            st->contest_conf = conf;
        }
        if (support_ms > st->contest_support_ms) {
            st->contest_support_ms = support_ms;
        }
        return;
    }
    /* Keep the stronger contender. */
    if (st->contest_leaf == EAR_CLS_NODE_NONE
        || conf > st->contest_conf
        || (conf == st->contest_conf && support_ms > st->contest_support_ms)) {
        st->contest_leaf = leaf;
        st->contest_conf = conf;
        st->contest_support_ms = support_ms;
    }
}

static bool end_correction_ok(const ear_cls_state_t *st)
{
    if (st->leaf == EAR_CLS_NODE_NONE || st->contest_leaf == EAR_CLS_NODE_NONE) {
        return false;
    }
    if (st->contest_leaf == st->leaf) {
        return false;
    }
    if (st->contest_conf < EAR_CLS_END_CORR_MIN_CONF) {
        return false;
    }
    if (st->contest_support_ms < EAR_CLS_END_CORR_MIN_SUPPORT_MS) {
        return false;
    }
    /* Inclusive margin with tiny epsilon against float rounding. */
    if (st->contest_conf + 1.0e-4f < st->conf_leaf + EAR_CLS_END_CORR_MARGIN) {
        return false;
    }
    const float need =
        EAR_CLS_END_CORR_SUPPORT_RATIO * (float)st->leaf_support_ms;
    if ((float)st->contest_support_ms + 0.5f < need) {
        return false;
    }
    return true;
}

static void try_set_leaf(
    ear_cls_state_t *st,
    ear_cls_node_id_t leaf,
    float conf,
    uint32_t support_ms
)
{
    if (!is_leaf(leaf)) {
        return;
    }
    conf = clamp01(conf);

    if (st->leaf != EAR_CLS_NODE_NONE && leaf != st->leaf) {
        /* Sibling evidence is contested; PERIODIC never swaps. END may. */
        note_contest(st, leaf, conf, support_ms);
        return;
    }

    const bool leaf_ok = conf >= EAR_CLS_LEAF_PERIODIC_MIN_CONF
        && support_ms >= EAR_CLS_LEAF_PERIODIC_MIN_SUPPORT_MS;
    if (!leaf_ok) {
        return;
    }

    if (st->leaf == EAR_CLS_NODE_NONE) {
        st->leaf = leaf;
        st->conf_leaf = conf;
        st->leaf_support_ms = support_ms;
        st->deepest = leaf;
        st->refine_count++;
        return;
    }

    /* Same leaf: allow confidence/support to strengthen. */
    if (conf > st->conf_leaf) {
        st->conf_leaf = conf;
    }
    if (support_ms > st->leaf_support_ms) {
        st->leaf_support_ms = support_ms;
    }
}

bool ear_cls_apply(
    ear_cls_state_t *st,
    const ear_cls_evidence_t *ev,
    ear_cls_phase_t phase,
    ear_cls_assessment_wire_t *out_wire
)
{
    if (st == NULL) {
        return false;
    }
    if (!st->initialized) {
        ear_cls_state_init(st);
    }
    if (ev == NULL) {
        return build_wire(st, out_wire);
    }

    const ear_cls_node_id_t before = st->deepest;

    if (ev->alarm_ok && clamp01(ev->alarm_confidence) >= EAR_CLS_ALARM_MIN_CONF) {
        const float c = clamp01(ev->alarm_confidence);
        if (st->deepest == EAR_CLS_NODE_NONE) {
            st->deepest = EAR_CLS_NODE_ALARM;
            st->conf_alarm = c;
            st->refine_count++;
        } else if (c > st->conf_alarm) {
            st->conf_alarm = c;
        }
    }

    if (st->deepest != EAR_CLS_NODE_NONE && ev->se_public_alert_ok
        && clamp01(ev->se_public_alert_confidence) >= EAR_CLS_SE_MIN_CONF) {
        const float c = clamp01(ev->se_public_alert_confidence);
        if (st->deepest == EAR_CLS_NODE_ALARM) {
            st->deepest = EAR_CLS_NODE_SE_PUBLIC_ALERT;
            st->conf_se = c;
            st->refine_count++;
        } else if (c > st->conf_se) {
            st->conf_se = c;
        }
    }

    if (st->deepest == EAR_CLS_NODE_SE_PUBLIC_ALERT || is_leaf(st->deepest)) {
        if (ev->faran_over_ok) {
            try_set_leaf(
                st,
                EAR_CLS_NODE_FARAN_OVER,
                ev->faran_over_confidence,
                ev->faran_over_support_ms
            );
        }
        if (ev->vma_ok) {
            try_set_leaf(
                st, EAR_CLS_NODE_VMA, ev->vma_confidence, ev->vma_support_ms
            );
        }
        /* Sibling correction is exceptional and END-only. */
        if (phase == EAR_CLS_PHASE_END && end_correction_ok(st)) {
            st->leaf = st->contest_leaf;
            st->conf_leaf = st->contest_conf;
            st->leaf_support_ms = st->contest_support_ms;
            st->deepest = st->leaf;
            st->correction_count++;
            st->contest_leaf = EAR_CLS_NODE_NONE;
            st->contest_conf = 0.0f;
            st->contest_support_ms = 0u;
        }
    }

    (void)before;
    return build_wire(st, out_wire);
}

bool ear_cls_snapshot(const ear_cls_state_t *st, ear_cls_assessment_wire_t *out_wire)
{
    return build_wire(st, out_wire);
}
