#include "sp_mic_window_job.h"

#include "sp_mic_window_job_slot.h"

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

static sp_mic_window_slot_pending_t s_pending;
static uint32_t s_overwrite_count;

void sp_mic_window_slot_reset_pending(void)
{
    memset(&s_pending, 0, sizeof(s_pending));
}

uint32_t sp_mic_window_slot_overwrite_count(void)
{
    return s_overwrite_count;
}

void sp_mic_window_slot_publish(const sp_mic_window_slot_pending_t *in)
{
    if (in == NULL) {
        return;
    }
    if (s_pending.pending) {
        s_overwrite_count++;
    }
    s_pending = *in;
    s_pending.pending = true;
}

bool sp_mic_window_slot_take(sp_mic_window_slot_pending_t *out)
{
    if (out == NULL || !s_pending.pending) {
        return false;
    }
    *out = s_pending;
    s_pending.pending = false;
    return true;
}

static bool slot_bank_usable(
    const sp_mic_window_slot_pending_t *snap,
    int32_t *(*banks)[2]
)
{
    if (snap == NULL || banks == NULL) {
        return false;
    }
    if (snap->buf_idx < 0 || snap->buf_idx > 1) {
        return false;
    }
    return banks[0][snap->buf_idx] != NULL;
}

static void slot_fill_pcm_job(
    const sp_mic_window_slot_pending_t *snap,
    int32_t *(*banks)[2],
    sp_mic_pcm_job_t *out
)
{
    memset(out, 0, sizeof(*out));
    out->full_scale_bits = snap->full_scale_bits;
    out->window_ms = snap->window_ms;
    out->window_end_mono_us = snap->window_end_mono_us;
    out->window_seq = snap->window_seq;
    out->pcm_channel_mask = snap->pcm_channel_mask;
    out->active_mic_count = snap->live_mic_count;
    for (uint8_t ch = 0u; ch < SP_MIC_COUNT_MAX; ++ch) {
        if ((snap->pcm_channel_mask & (uint8_t)(1u << ch)) == 0u) {
            continue;
        }
        if (banks[ch][snap->buf_idx] == NULL) {
            continue;
        }
        out->samples_ch[ch] = banks[ch][snap->buf_idx];
        out->sample_count_ch[ch] = snap->sample_count_ch[ch];
        if (ch == 0u) {
            out->samples = out->samples_ch[0];
            out->sample_count = out->sample_count_ch[0];
        }
    }
}

bool sp_mic_window_slot_project_pcm(
    const sp_mic_window_slot_pending_t *snap,
    int32_t *(*banks)[2],
    sp_mic_pcm_job_t *out
)
{
    if (!slot_bank_usable(snap, banks)) {
        return false;
    }
    if (out == NULL) {
        return true;
    }
    slot_fill_pcm_job(snap, banks, out);
    return true;
}

bool sp_mic_window_slot_project_window(
    const sp_mic_window_slot_pending_t *snap,
    int32_t *(*banks)[2],
    uint32_t sample_rate_hz,
    sp_mic_window_job_t *out
)
{
    if (!slot_bank_usable(snap, banks)) {
        return false;
    }
    if (out == NULL) {
        return true;
    }
    memset(out, 0, sizeof(*out));
    slot_fill_pcm_job(snap, banks, &out->pcm);
    uint8_t live = snap->live_mic_count;
    if (live < 1u || live > SP_MIC_COUNT_MAX) {
        live = 1u;
    }
    (void)sp_mic_window_job_set_channels_from_views(
        out, live, sample_rate_hz, snap->views
    );
    return true;
}
