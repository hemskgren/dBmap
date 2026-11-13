#include "sp_mic_window_job.h"

#include <string.h>

int sp_mic_window_job_set_channels_from_views(
    sp_mic_window_job_t *job,
    uint8_t live_mic_count,
    uint32_t sample_rate_hz,
    const sp_mic_window_view_t *views
)
{
    if (job == NULL || views == NULL) {
        return SP_MIC_API_ERR_INVALID_ARG;
    }
    if (live_mic_count < 1u || live_mic_count > SP_MIC_COUNT_MAX) {
        return SP_MIC_API_ERR_INVALID_ARG;
    }

    job->live_mic_count = live_mic_count;
    job->channel_mask = 0u;
    memset(job->channels, 0, sizeof(job->channels));

    for (uint8_t i = 0; i < live_mic_count; i++) {
        if (views[i].measured_at_us <= 0) {
            /* Not yet published / empty — leave bit clear (distinguish from zero). */
            continue;
        }
        if (sp_mic_fill_channel(i, live_mic_count, &views[i], sample_rate_hz, &job->channels[i])
            != SP_MIC_API_OK) {
            return SP_MIC_API_ERR_INVALID_ARG;
        }
        job->channel_mask = (uint8_t)(job->channel_mask | (1u << i));
    }
    return SP_MIC_API_OK;
}

bool sp_mic_window_job_channels_coherent(const sp_mic_window_job_t *job)
{
    if (job == NULL) {
        return false;
    }
    if (job->pcm.window_seq == 0u) {
        return false;
    }
    for (uint8_t i = 0; i < SP_MIC_COUNT_MAX; i++) {
        if ((job->channel_mask & (1u << i)) == 0u) {
            continue;
        }
        const sp_mic_channel_t *ch = &job->channels[i];
        if (!ch->valid) {
            return false;
        }
        if (ch->window_seq != job->pcm.window_seq) {
            return false;
        }
        if (ch->measured_at_us != job->pcm.window_end_mono_us) {
            return false;
        }
    }
    return true;
}
