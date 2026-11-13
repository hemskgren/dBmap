#include "ae_gap.h"

#include <stddef.h>

ae_window_gap_t ae_window_gap_eval(
    uint32_t last_published_seq,
    uint32_t job_window_seq,
    uint32_t overwrite_counter_now,
    uint32_t *inout_last_overwrite_seen
)
{
    ae_window_gap_t out;
    out.seq_gap = false;
    out.overwrite_delta = 0u;
    out.gap_before = false;

    if (inout_last_overwrite_seen == NULL) {
        return out;
    }

    /* Modular uint32 subtraction handles wrap correctly. */
    out.overwrite_delta =
        overwrite_counter_now - *inout_last_overwrite_seen;
    *inout_last_overwrite_seen = overwrite_counter_now;

    if (last_published_seq != 0u
        && job_window_seq > last_published_seq + 1u) {
        out.seq_gap = true;
    }

    out.gap_before = out.seq_gap || (out.overwrite_delta > 0u);
    return out;
}
