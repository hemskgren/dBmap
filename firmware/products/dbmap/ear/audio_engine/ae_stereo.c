#include "ae_stereo.h"

#include <stddef.h>

ae_stereo_eval_t ae_stereo_eval(
    const ae_align_result_t *align,
    bool expected_stereo,
    uint8_t pcm_channel_mask,
    size_t pcm_n0,
    size_t pcm_n1
)
{
    ae_stereo_eval_t out;
    out.state = AE_STEREO_NONE;
    out.pair_metrics_ok = false;
    out.pcm_channel_mask = pcm_channel_mask;
    out.pcm_n0 = (uint32_t)pcm_n0;
    out.pcm_n1 = (uint32_t)pcm_n1;

    if (align == NULL) {
        return out;
    }

    out.pair_metrics_ok = align->pair_aligned && align->pair.valid;

    const bool pcm0 = (pcm_channel_mask & 0x01u) != 0u && pcm_n0 > 0u;
    const bool pcm1 = (pcm_channel_mask & 0x02u) != 0u && pcm_n1 > 0u;

    if (!expected_stereo) {
        out.state = AE_STEREO_NONE;
        return out;
    }

    if ((align->mismatch_flags & (AE_ALIGN_MISMATCH_MIC0 | AE_ALIGN_MISMATCH_MIC1))
        != 0u) {
        out.state = AE_STEREO_MISALIGNED;
        return out;
    }

    if (!align->pair_aligned) {
        out.state = AE_STEREO_MONO_FALLBACK;
        return out;
    }

    /* Pair metrics OK — require dual PCM for full stereo foundation. */
    if (!pcm0 || !pcm1) {
        out.state = AE_STEREO_PCM_INCOMPLETE;
        return out;
    }

    if (align->pair.stereo_degraded) {
        out.state = AE_STEREO_DEGRADED;
        return out;
    }

    out.state = AE_STEREO_OK;
    return out;
}

const char *ae_stereo_state_str(ae_stereo_state_t state)
{
    switch (state) {
    case AE_STEREO_NONE:
        return "none";
    case AE_STEREO_OK:
        return "ok";
    case AE_STEREO_MONO_FALLBACK:
        return "mono_fallback";
    case AE_STEREO_MISALIGNED:
        return "misaligned";
    case AE_STEREO_DEGRADED:
        return "degraded";
    case AE_STEREO_PCM_INCOMPLETE:
        return "pcm_incomplete";
    default:
        return "unknown";
    }
}
