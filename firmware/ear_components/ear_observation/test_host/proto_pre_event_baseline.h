/**
 * Host-only prototype: short pre-event RMS baseline (not production firmware).
 *
 * Rolling usable-window RMS history for freeze-at-START local END evaluation.
 * Independent of the 180 s Reported-State background profile.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_PRE_EVENT_BASELINE_CAP 5
#define PROTO_PRE_EVENT_BASELINE_MIN_SAMPLES 3

typedef struct {
    float rms_dbfs;
    int64_t t_end_mono_us;
    bool valid;
} proto_pre_event_sample_t;

typedef struct {
    proto_pre_event_sample_t entries[PROTO_PRE_EVENT_BASELINE_CAP];
    uint32_t head;  /* next write */
    uint32_t count;
    /** Drop samples older than this before freeze; 0 disables. */
    uint32_t stale_max_ms;
    bool frozen;
    bool reliable;
    float frozen_baseline_dbfs;
    uint32_t frozen_sample_count;
    bool used_fallback; /* informational: set by relative detector when absolute used */
} proto_pre_event_baseline_t;

void proto_pre_event_baseline_init(proto_pre_event_baseline_t *b, uint32_t stale_max_ms);

/** Push one usable pre-START window. No-op while frozen. */
void proto_pre_event_baseline_push(
    proto_pre_event_baseline_t *b,
    float rms_dbfs,
    int64_t t_end_mono_us
);

/**
 * Freeze median of valid samples with t_end_mono_us <= episode_start_us.
 * Excludes the START-triggering window and later event windows when episode_start_us
 * is the window_start of the first above-enter sample.
 * Returns true if reliable (>= MIN_SAMPLES after stale filter).
 */
bool proto_pre_event_baseline_freeze(
    proto_pre_event_baseline_t *b,
    int64_t episode_start_us
);

/** Clear freeze and empty the ring (resume collection after END). */
void proto_pre_event_baseline_clear(proto_pre_event_baseline_t *b);

float proto_pre_event_baseline_median(const float *values, uint32_t n);

#ifdef __cplusplus
}
#endif
