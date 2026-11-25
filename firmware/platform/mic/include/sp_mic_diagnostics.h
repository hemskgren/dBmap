#pragma once

/* Fixed-size, UART-only diagnostic accounting. */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t min_us;
    uint64_t max_us;
    uint64_t last_us;
    uint64_t count;
} sp_mic_time_diag_t;

static inline void sp_mic_time_diag_record(sp_mic_time_diag_t *diag, int64_t start, int64_t end)
{
    if (start < 0 || end < start) {
        return;
    }
    const uint64_t elapsed = (uint64_t)(end - start);
    if (diag->count == 0 || elapsed < diag->min_us) {
        diag->min_us = elapsed;
    }
    if (elapsed > diag->max_us) {
        diag->max_us = elapsed;
    }
    diag->last_us = elapsed;
    diag->count++;
}

typedef struct {
    uint64_t read_errors;
    uint64_t timeout_reads;
    uint64_t empty_reads;
    uint64_t partial_reads;
    uint64_t incomplete_stereo;
    uint64_t capped_windows;
} sp_mic_din_diag_t;

static inline void sp_mic_din_diag_add(sp_mic_din_diag_t *total, const sp_mic_din_diag_t *window)
{
    total->read_errors += window->read_errors;
    total->timeout_reads += window->timeout_reads;
    total->empty_reads += window->empty_reads;
    total->partial_reads += window->partial_reads;
    total->incomplete_stereo += window->incomplete_stereo;
    total->capped_windows += window->capped_windows;
}
