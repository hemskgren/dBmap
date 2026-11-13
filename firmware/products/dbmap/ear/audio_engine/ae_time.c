#include "ae_time.h"

#include <stddef.h>

void ae_time_cursor_init(ae_time_cursor_t *cur)
{
    if (cur == NULL) {
        return;
    }
    cur->have_prev = false;
    cur->prev_window_seq = 0u;
    cur->prev_window_end_mono_us = 0;
}

ae_time_status_t ae_time_validate_publish(
    const ae_time_cursor_t *cur,
    const ae_signature_t *sig
)
{
    if (sig == NULL) {
        return AE_TIME_BAD_IDENTITY;
    }
    if (sig->schema_version != AE_SIGNATURE_SCHEMA_VERSION || sig->window_seq == 0u) {
        return AE_TIME_BAD_IDENTITY;
    }
    if (sig->window_end_mono_us <= 0) {
        return AE_TIME_MISSING;
    }
    if (cur == NULL || !cur->have_prev) {
        return AE_TIME_OK;
    }
    if (sig->window_seq == cur->prev_window_seq) {
        return AE_TIME_DUPLICATE_SEQ;
    }
    if (sig->window_seq < cur->prev_window_seq) {
        return AE_TIME_SEQ_REGRESS;
    }
    if (sig->window_end_mono_us <= cur->prev_window_end_mono_us) {
        return AE_TIME_NON_MONOTONIC;
    }
    return AE_TIME_OK;
}

void ae_time_cursor_accept(ae_time_cursor_t *cur, const ae_signature_t *sig)
{
    if (cur == NULL || sig == NULL) {
        return;
    }
    cur->have_prev = true;
    cur->prev_window_seq = sig->window_seq;
    cur->prev_window_end_mono_us = sig->window_end_mono_us;
}

const char *ae_time_status_str(ae_time_status_t st)
{
    switch (st) {
    case AE_TIME_OK:
        return "ok";
    case AE_TIME_MISSING:
        return "missing";
    case AE_TIME_NON_MONOTONIC:
        return "non_monotonic";
    case AE_TIME_DUPLICATE_SEQ:
        return "duplicate_seq";
    case AE_TIME_SEQ_REGRESS:
        return "seq_regress";
    case AE_TIME_BAD_IDENTITY:
        return "bad_identity";
    default:
        return "unknown";
    }
}
