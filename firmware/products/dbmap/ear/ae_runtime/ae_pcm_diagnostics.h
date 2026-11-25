#pragma once

#include "sp_mic_diagnostics.h"
#include "sp_mic_window_job.h"

typedef struct {
    uint32_t captured;
    size_t retained;
    uint32_t discarded;
    uint32_t discard_max;
    uint64_t discard_total;
} ae_pcm_span_diag_t;

typedef struct {
    ae_pcm_span_diag_t channels[SP_MIC_COUNT_MAX];
    sp_mic_time_diag_t queue_age;
    sp_mic_time_diag_t processing_age;
    uint64_t checked;
    uint64_t stale_before_access;
    uint64_t reuse_during_access;
    uint32_t software_missing_frames;
    uint16_t carried_frames;
    uint8_t span_end;
    uint8_t captured_mask;
    uint8_t comparable_mask;
    uint8_t live_count;
} ae_pcm_diagnostics_t;

static inline void ae_pcm_diag_record_spans(
    ae_pcm_diagnostics_t *diag, const sp_mic_window_job_t *job
)
{
    diag->software_missing_frames = job->software_missing_frames;
    diag->carried_frames = job->carried_frames;
    diag->span_end = job->span_end;
    diag->live_count = job->live_mic_count;
    diag->captured_mask = job->channel_mask;
    diag->comparable_mask = 0;
    for (uint8_t ch = 0; ch < SP_MIC_COUNT_MAX; ++ch) {
        ae_pcm_span_diag_t *span = &diag->channels[ch];
        span->captured = job->channels[ch].sample_count;
        span->retained = job->pcm.sample_count_ch[ch];
        span->discarded = 0;
        if (sp_mic_window_job_cap_discarded(job, ch, &span->discarded)) {
            diag->comparable_mask |= (uint8_t)(1u << ch);
            span->discard_total += span->discarded;
            if (span->discarded > span->discard_max) {
                span->discard_max = span->discarded;
            }
        }
    }
}

static inline void ae_pcm_diag_record_check(
    ae_pcm_diagnostics_t *diag, uint32_t expected, uint32_t before, uint32_t after
)
{
    diag->checked++;
    if (before != expected) {
        diag->stale_before_access++;
    }
    if (after != before) {
        diag->reuse_during_access++;
    }
}
