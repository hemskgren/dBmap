/**
 * Audio Engine acquisition-time contract (CE clock).
 *
 * Rules:
 * - ``window_end_mono_us`` is acquisition-tied (Platform window / PCM job end).
 * - Do not substitute task, analyze, or MQTT time for this field.
 * - Published history is ordered by strictly increasing ``window_end_mono_us``.
 * - ``window_seq`` must not regress or duplicate relative to the last accepted
 *   publish (forward gaps are allowed and may be marked AE_QUALITY_GAP).
 */
#pragma once

#include "ae_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AE_TIME_OK = 0,
    AE_TIME_MISSING = 1,         /* window_end_mono_us <= 0 */
    AE_TIME_NON_MONOTONIC = 2,   /* end_us <= previous accepted end_us */
    AE_TIME_DUPLICATE_SEQ = 3,   /* same window_seq as previous accepted */
    AE_TIME_SEQ_REGRESS = 4,     /* window_seq < previous accepted */
    AE_TIME_BAD_IDENTITY = 5,    /* seq==0 or schema mismatch */
} ae_time_status_t;

typedef struct {
    bool have_prev;
    uint32_t prev_window_seq;
    int64_t prev_window_end_mono_us;
} ae_time_cursor_t;

void ae_time_cursor_init(ae_time_cursor_t *cur);

/**
 * Validate ``sig`` against the last accepted cursor (if any).
 * Does not mutate ``sig`` or ``cur``.
 */
ae_time_status_t ae_time_validate_publish(
    const ae_time_cursor_t *cur,
    const ae_signature_t *sig
);

/** Advance cursor after a successful publish. */
void ae_time_cursor_accept(ae_time_cursor_t *cur, const ae_signature_t *sig);

const char *ae_time_status_str(ae_time_status_t st);

#ifdef __cplusplus
}
#endif
