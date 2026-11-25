/** Frame accounting over caller-owned I2S scratch. No sample storage. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sp_mic_window_job.h"

/* Sized for the fixed, at-most UINT16_MAX byte, per-DIN read scratch. */
typedef struct {
    /* Software ordinals only. Each discarded fragment reserves one ordinal. */
    uint64_t first_frame;
    uint64_t next_frame;
    uint16_t offset;
    uint16_t frames;
    uint8_t skip_bytes;
} sp_mic_frame_cursor_t;

typedef struct {
    uint16_t discarded_frames;
    bool incomplete_frame;
} sp_mic_frame_read_t;

/* Do not stitch a truncated frame across reads. Skip its remaining bytes. */
static inline sp_mic_frame_read_t sp_mic_frame_read(
    sp_mic_frame_cursor_t *cursor, uint16_t bytes, uint8_t frame_bytes, bool usable
)
{
    const uint8_t prefix = bytes < cursor->skip_bytes ? (uint8_t)bytes : cursor->skip_bytes;
    cursor->skip_bytes -= prefix;
    const uint16_t rest = bytes - prefix;
    const uint16_t frames = rest / frame_bytes;
    const uint8_t tail = rest % frame_bytes;
    if (tail != 0u) {
        cursor->skip_bytes = frame_bytes - tail;
    }
    cursor->first_frame = cursor->next_frame;
    cursor->next_frame += frames + (tail != 0u);
    cursor->offset = prefix;
    cursor->frames = usable ? frames : 0u;
    return (sp_mic_frame_read_t){
        .discarded_frames = usable ? 0u : frames,
        .incomplete_frame = tail != 0u,
    };
}

static inline uint32_t sp_mic_frame_group(
    const sp_mic_frame_cursor_t *cursors, uint8_t din_count, uint32_t remaining
)
{
    for (uint8_t din = 0; din < din_count; ++din) {
        if (cursors[din].frames < remaining) {
            remaining = cursors[din].frames;
        }
    }
    return remaining;
}

static inline void sp_mic_frame_consume(
    sp_mic_frame_cursor_t *cursor, uint16_t frames, uint8_t frame_bytes
)
{
    cursor->offset += frames * frame_bytes;
    cursor->frames -= frames;
    cursor->first_frame += frames;
}

/* Drop unmatched prefixes only when both DINs have a known software ordinal.
 * A missing/timeout read alone supplies no evidence about physical loss. */
static inline uint16_t sp_mic_frame_align(
    sp_mic_frame_cursor_t *cursor, uint64_t group_start, uint8_t frame_bytes
)
{
    if (cursor->first_frame >= group_start) {
        return 0u;
    }
    const uint64_t delta = group_start - cursor->first_frame;
    const uint16_t discard = delta < cursor->frames ? (uint16_t)delta : cursor->frames;
    sp_mic_frame_consume(cursor, discard, frame_bytes);
    return discard;
}

static inline uint32_t sp_mic_frame_slot(const uint8_t *slot)
{
    uint32_t word;
    memcpy(&word, slot, sizeof(word));
    return word;
}

/** Reject fractional spans and allocations that would overflow, on either ABI. */
static inline bool sp_mic_frame_target(uint32_t rate, uint32_t ms, uint32_t *out)
{
    const uint64_t product = (uint64_t)rate * ms;
    const uint64_t frames = product / 1000u;
    if (product % 1000u != 0u || frames == 0u || frames > UINT32_MAX
        || frames > SIZE_MAX / sizeof(int32_t)) {
        return false;
    }
    *out = (uint32_t)frames;
    return true;
}

static inline sp_mic_span_end_t sp_mic_frame_stop(
    int64_t now, int64_t last_progress, int64_t deadline, int64_t stall_us
)
{
    if (now - last_progress >= stall_us) {
        return SP_MIC_SPAN_STALLED;
    }
    return now >= deadline ? SP_MIC_SPAN_DEADLINE : SP_MIC_SPAN_COMPLETE;
}
