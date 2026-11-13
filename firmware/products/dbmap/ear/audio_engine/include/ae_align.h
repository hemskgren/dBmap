/**
 * PCM job ↔ channel measurement association (host-testable).
 *
 * Platform measure_window stamps the same window_seq and measured_at on
 * channel snapshots and the PCM pending job. AE must verify that equality
 * before combining; never assume get_channel() matches a taken PCM job.
 */
#pragma once

#include "ae_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t window_seq;
    int64_t measured_at_us;
    bool present; /* API returned a channel */
    bool has_samples; /* sample_count > 0 */
    float rms_dbfs;
    float peak_dbfs;
    bool degraded; /* e.g. disconnected_suspected / transport fault */
} ae_channel_snap_t;

typedef struct {
    uint32_t pcm_window_seq;
    int64_t pcm_end_mono_us;
    bool pcm_seq_valid; /* pcm_window_seq != 0 */
    ae_channel_snap_t mic0;
    ae_channel_snap_t mic1;
} ae_align_input_t;

typedef struct {
    bool mic0_aligned;
    bool mic1_aligned;
    bool pair_aligned; /* both aligned to PCM window */
    uint32_t mismatch_flags; /* bit0 mic0 mismatch, bit1 mic1 mismatch */
    ae_pair_metrics_t pair;
} ae_align_result_t;

#define AE_ALIGN_MISMATCH_MIC0 (1u << 0)
#define AE_ALIGN_MISMATCH_MIC1 (1u << 1)

/**
 * Associate channel snapshots with a PCM job.
 * Pair metrics are filled only when both mics align to the same PCM window
 * and both have samples. Missing channels are not treated as zero signal.
 */
void ae_align_channels(const ae_align_input_t *in, ae_align_result_t *out);

/** True when channel timing+seq match the PCM job. */
bool ae_channel_matches_pcm(
    uint32_t pcm_seq,
    int64_t pcm_end_us,
    bool pcm_seq_valid,
    const ae_channel_snap_t *ch
);

#ifdef __cplusplus
}
#endif
