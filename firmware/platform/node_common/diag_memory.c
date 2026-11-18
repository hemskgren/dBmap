#include "sp_diag_memory.h"

#include <string.h>

void sp_diag_mem_reporter_init(sp_diag_mem_reporter_t *rep)
{
    if (rep == NULL) {
        return;
    }
    rep->last_reported_fp = 0u;
}

static uint32_t quant(uint32_t bytes)
{
    return bytes >> SP_DIAG_MEM_QUANT_SHIFT;
}

static uint32_t build_fingerprint(const sp_diag_mem_anomaly_t *a)
{
    uint32_t fp = a->kinds;
    if (a->kinds & SP_DIAG_MEM_KIND_FREE_HEAP) {
        fp ^= quant(a->free_heap) << 1;
    }
    if (a->kinds & SP_DIAG_MEM_KIND_MIN_FREE_HEAP) {
        fp ^= quant(a->min_free_heap) << 2;
    }
    if (a->kinds & (SP_DIAG_MEM_KIND_FREE_INTERNAL | SP_DIAG_MEM_KIND_FRAGMENTATION)) {
        fp ^= quant(a->free_internal) << 3;
    }
    if (a->kinds & SP_DIAG_MEM_KIND_MIN_FREE_INTERNAL) {
        fp ^= quant(a->min_free_internal) << 4;
    }
    if (a->kinds
        & (SP_DIAG_MEM_KIND_LARGEST_INTERNAL | SP_DIAG_MEM_KIND_FRAGMENTATION)) {
        fp ^= quant(a->largest_internal) << 5;
    }
    if (a->kinds & SP_DIAG_MEM_KIND_STACK_HWM) {
        fp ^= quant(a->stack_hwm_min_bytes) << 6;
    }
    /* kinds==0 must yield 0; avoid accidental non-zero from empty XOR. */
    if (a->kinds == 0u) {
        return 0u;
    }
    /* Ensure non-zero fingerprint whenever anomalous (kinds alone can be small). */
    if (fp == 0u) {
        fp = a->kinds | 0x80000000u;
    }
    return fp;
}

void sp_diag_mem_evaluate(const sp_diag_mem_sample_t *in, sp_diag_mem_anomaly_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (in == NULL) {
        return;
    }

    out->free_heap = in->free_heap;
    out->min_free_heap = in->min_free_heap;
    out->free_internal = in->free_internal;
    out->min_free_internal = in->min_free_internal;
    out->largest_internal = in->largest_internal;
    out->stack_hwm_min_bytes = in->stack_hwm_min_bytes;

    if (in->free_heap < SP_DIAG_MEM_FREE_HEAP_LOW_BYTES) {
        out->kinds |= SP_DIAG_MEM_KIND_FREE_HEAP;
    }
    if (in->min_free_heap < SP_DIAG_MEM_MIN_FREE_HEAP_LOW_BYTES) {
        out->kinds |= SP_DIAG_MEM_KIND_MIN_FREE_HEAP;
    }
    if (in->free_internal < SP_DIAG_MEM_FREE_INTERNAL_LOW_BYTES) {
        out->kinds |= SP_DIAG_MEM_KIND_FREE_INTERNAL;
    }
    if (in->min_free_internal < SP_DIAG_MEM_MIN_FREE_INTERNAL_LOW_BYTES) {
        out->kinds |= SP_DIAG_MEM_KIND_MIN_FREE_INTERNAL;
    }

    if (in->largest_internal < SP_DIAG_MEM_LARGEST_INTERNAL_LOW_BYTES) {
        if (in->free_internal >= SP_DIAG_MEM_FRAG_FREE_INTERNAL_FLOOR_BYTES
            && in->largest_internal < (in->free_internal / 2u)) {
            out->kinds |= SP_DIAG_MEM_KIND_FRAGMENTATION;
        } else {
            out->kinds |= SP_DIAG_MEM_KIND_LARGEST_INTERNAL;
        }
    }

    if (in->stack_sampled && in->stack_hwm_min_bytes < SP_DIAG_MEM_STACK_HWM_LOW_BYTES) {
        out->kinds |= SP_DIAG_MEM_KIND_STACK_HWM;
    }

    out->fingerprint = build_fingerprint(out);
}

bool sp_diag_mem_should_report(sp_diag_mem_reporter_t *rep, const sp_diag_mem_anomaly_t *anom)
{
    if (rep == NULL || anom == NULL) {
        return false;
    }
    if (anom->kinds == 0u || anom->fingerprint == 0u) {
        /* Healthy: allow a future anomaly with the same bucket to report. */
        rep->last_reported_fp = 0u;
        return false;
    }
    return anom->fingerprint != rep->last_reported_fp;
}

void sp_diag_mem_note_published(sp_diag_mem_reporter_t *rep, uint32_t fingerprint)
{
    if (rep == NULL || fingerprint == 0u) {
        return;
    }
    rep->last_reported_fp = fingerprint;
}
