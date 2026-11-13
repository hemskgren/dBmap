#include "ae_align.h"

#include <string.h>

bool ae_channel_matches_pcm(
    uint32_t pcm_seq,
    int64_t pcm_end_us,
    bool pcm_seq_valid,
    const ae_channel_snap_t *ch
)
{
    if (ch == NULL || !ch->present || ch->measured_at_us <= 0) {
        return false;
    }
    if (ch->measured_at_us != pcm_end_us) {
        return false;
    }
    if (pcm_seq_valid && ch->window_seq != pcm_seq) {
        return false;
    }
    return true;
}

void ae_align_channels(const ae_align_input_t *in, ae_align_result_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (in == NULL) {
        return;
    }

    out->mic0_aligned = ae_channel_matches_pcm(
        in->pcm_window_seq, in->pcm_end_mono_us, in->pcm_seq_valid, &in->mic0
    );
    out->mic1_aligned = ae_channel_matches_pcm(
        in->pcm_window_seq, in->pcm_end_mono_us, in->pcm_seq_valid, &in->mic1
    );

    if (in->mic0.present && !out->mic0_aligned) {
        out->mismatch_flags |= AE_ALIGN_MISMATCH_MIC0;
    }
    if (in->mic1.present && !out->mic1_aligned) {
        out->mismatch_flags |= AE_ALIGN_MISMATCH_MIC1;
    }

    if (out->mic0_aligned && out->mic1_aligned
        && in->mic0.has_samples && in->mic1.has_samples) {
        out->pair_aligned = true;
        out->pair.valid = true;
        out->pair.mic0_rms_dbfs = in->mic0.rms_dbfs;
        out->pair.mic1_rms_dbfs = in->mic1.rms_dbfs;
        out->pair.mic0_peak_dbfs = in->mic0.peak_dbfs;
        out->pair.mic1_peak_dbfs = in->mic1.peak_dbfs;
        out->pair.mic_mask = 0x03u;
        out->pair.stereo_degraded = in->mic0.degraded || in->mic1.degraded;
    }
}
