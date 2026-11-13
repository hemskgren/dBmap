/**
 * Stereo window validity for AE (metrics + PCM foundation). No TDOA/DOA.
 */
#pragma once

#include "ae_align.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AE_STEREO_NONE = 0,           /* mono path / no second channel expected */
    AE_STEREO_OK = 1,             /* both aligned, samples present, PCM ok */
    AE_STEREO_MONO_FALLBACK = 2,  /* second channel absent (mask/PCM) */
    AE_STEREO_MISALIGNED = 3,     /* present but seq/time mismatch */
    AE_STEREO_DEGRADED = 4,       /* pair ok but degraded mic status */
    AE_STEREO_PCM_INCOMPLETE = 5, /* channel metrics ok but PCM mask/counts incomplete */
} ae_stereo_state_t;

typedef struct {
    ae_stereo_state_t state;
    bool pair_metrics_ok;
    uint8_t pcm_channel_mask;
    uint32_t pcm_n0;
    uint32_t pcm_n1;
} ae_stereo_eval_t;

/**
 * Combine align result with PCM channel availability.
 * expected_stereo: true when live acquisition is stereo (mic_count ≥ 2).
 * Never treats missing/mismatched data as AE_STEREO_OK.
 */
ae_stereo_eval_t ae_stereo_eval(
    const ae_align_result_t *align,
    bool expected_stereo,
    uint8_t pcm_channel_mask,
    size_t pcm_n0,
    size_t pcm_n1
);

const char *ae_stereo_state_str(ae_stereo_state_t state);

#ifdef __cplusplus
}
#endif
