#pragma once

/**
 * Anomaly-only memory / stack diagnostics (Sensor Platform).
 *
 * Pure evaluation + report-once logic. Device code samples ESP-IDF heap /
 * stack APIs and feeds sp_diag_mem_evaluate(); host tests inject samples.
 *
 * Wire: optional nested diag.mem object on health/keepalive only when an
 * anomaly should be reported. Healthy samples produce no fields. Unchanged
 * anomalies are not re-attached until the fingerprint changes or the
 * anomaly clears and returns.
 *
 * Do not expose an internal memory_health enum on the wire.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Threshold rationale (ESP32-S3 N16R8 / IDF heap; internal DRAM is scarce
 * under SPIRAM_USE_MALLOC; combined free_heap stays large):
 *
 * - FREE / MIN_FREE_HEAP: soft floor on combined heap; catches severe
 *   exhaustion even when SPIRAM is present.
 * - FREE / MIN_FREE_INTERNAL: actionable DRAM pressure before alloc fail.
 * - LARGEST_INTERNAL: contiguous alloc risk; with free_internal still above
 *   the frag floor, signals fragmentation rather than simple exhaustion.
 * - STACK_HWM: FreeRTOS high-water mark remaining bytes for tracked tasks.
 * - QUANT_SHIFT: 4 KiB buckets so small jitter does not re-report.
 */
#define SP_DIAG_MEM_FREE_HEAP_LOW_BYTES         (48u * 1024u)
#define SP_DIAG_MEM_MIN_FREE_HEAP_LOW_BYTES     (32u * 1024u)
#define SP_DIAG_MEM_FREE_INTERNAL_LOW_BYTES     (16u * 1024u)
#define SP_DIAG_MEM_MIN_FREE_INTERNAL_LOW_BYTES (8u * 1024u)
#define SP_DIAG_MEM_LARGEST_INTERNAL_LOW_BYTES  (8u * 1024u)
#define SP_DIAG_MEM_FRAG_FREE_INTERNAL_FLOOR_BYTES (24u * 1024u)
#define SP_DIAG_MEM_STACK_HWM_LOW_BYTES         512u
#define SP_DIAG_MEM_QUANT_SHIFT                 12u

enum {
    SP_DIAG_MEM_KIND_FREE_HEAP = 1u << 0,
    SP_DIAG_MEM_KIND_MIN_FREE_HEAP = 1u << 1,
    SP_DIAG_MEM_KIND_FREE_INTERNAL = 1u << 2,
    SP_DIAG_MEM_KIND_MIN_FREE_INTERNAL = 1u << 3,
    /** Largest block low while free_internal still above frag floor. */
    SP_DIAG_MEM_KIND_FRAGMENTATION = 1u << 4,
    /** Largest block low together with low free (alloc risk). */
    SP_DIAG_MEM_KIND_LARGEST_INTERNAL = 1u << 5,
    SP_DIAG_MEM_KIND_STACK_HWM = 1u << 6,
};

typedef struct {
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint32_t free_internal;
    uint32_t min_free_internal;
    uint32_t largest_internal;
    /** Minimum stack HWM among sampled tasks (bytes). */
    uint32_t stack_hwm_min_bytes;
    bool stack_sampled;
} sp_diag_mem_sample_t;

typedef struct {
    uint32_t kinds;
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint32_t free_internal;
    uint32_t min_free_internal;
    uint32_t largest_internal;
    uint32_t stack_hwm_min_bytes;
    uint32_t fingerprint;
} sp_diag_mem_anomaly_t;

typedef struct {
    uint32_t last_reported_fp;
} sp_diag_mem_reporter_t;

void sp_diag_mem_reporter_init(sp_diag_mem_reporter_t *rep);

/** Classify sample into anomaly kinds; fingerprint 0 when healthy. */
void sp_diag_mem_evaluate(const sp_diag_mem_sample_t *in, sp_diag_mem_anomaly_t *out);

/**
 * True when kinds != 0 and fingerprint differs from last successful report.
 * When healthy, resets last_reported_fp so a later anomaly reports again.
 */
bool sp_diag_mem_should_report(sp_diag_mem_reporter_t *rep, const sp_diag_mem_anomaly_t *anom);

/** Call after a successful health/keepalive publish that included this fp. */
void sp_diag_mem_note_published(sp_diag_mem_reporter_t *rep, uint32_t fingerprint);

#ifdef __cplusplus
}
#endif
