#include "sp_mic_window_job.h"

#include "sp_mic_window_job_slot.h"

#include <string.h>

bool sp_mic_window_job_cap_discarded(
    const sp_mic_window_job_t *job, uint8_t channel, uint32_t *out
)
{
    if (job == NULL || out == NULL || channel >= SP_MIC_COUNT_MAX
        || (job->channel_mask & (1u << channel)) == 0u
        || (job->pcm.pcm_channel_mask & (1u << channel)) == 0u
        || job->pcm.samples_ch[channel] == NULL) {
        return false;
    }
    const sp_mic_channel_t *capture = &job->channels[channel];
    const size_t retained = job->pcm.sample_count_ch[channel];
    if (capture->window_seq != job->pcm.window_seq
        || capture->measured_at_us != job->pcm.window_end_mono_us
        || retained == 0u || retained > capture->sample_count) {
        return false;
    }
    *out = capture->sample_count - (uint32_t)retained;
    return true;
}

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

enum { BANK_FREE, BANK_FILLING, BANK_PENDING, BANK_LEASED };

static sp_mic_window_slot_pending_t s_pending;
static uint32_t s_overwrite_count;
static sp_mic_window_diag_t s_diag;
static uint64_t s_last_lease;
static uint8_t s_bank_state[2];
static uint8_t s_preferred_bank;

static void count_slot_event(uint32_t *count)
{
    if (*count == UINT32_MAX) {
        s_diag.saturated = true;
    } else {
        ++*count;
    }
}

static bool bank_has_state(int bank, uint8_t state)
{
    return bank >= 0 && bank < 2 && s_bank_state[bank] == state;
}

void sp_mic_window_slot_get_diag(sp_mic_window_diag_t *out)
{
    *out = s_diag;
    out->pending = s_pending.pending;
}

static int retire_pending(void)
{
    const int bank = s_pending.buf_idx;
    if (!bank_has_state(bank, BANK_PENDING)) {
        count_slot_event(&s_diag.ownership_violations);
        return -1;
    }
    s_pending.pending = false;
    s_bank_state[bank] = BANK_FREE;
    return bank;
}

void sp_mic_window_slot_reset_pending(void)
{
    if (s_pending.pending && retire_pending() >= 0) {
        count_slot_event(&s_diag.reset_discards);
    }
}

uint32_t sp_mic_window_slot_overwrite_count(void)
{
    return s_overwrite_count;
}

static int replace_pending(void)
{
    const int bank = retire_pending();
    if (bank >= 0) {
        ++s_overwrite_count;
        count_slot_event(&s_diag.overwrites);
    }
    return bank;
}

int sp_mic_window_slot_begin_fill(void)
{
    int bank = s_preferred_bank;
    if (!bank_has_state(bank, BANK_FREE)) {
        bank ^= 1;
    }
    if (!bank_has_state(bank, BANK_FREE)) {
        bank = s_pending.pending ? replace_pending() : -1;
        if (bank < 0) {
            count_slot_event(&s_diag.no_bank_drops);
            return -1;
        }
        count_slot_event(&s_diag.pending_retired);
    }
    s_bank_state[bank] = BANK_FILLING;
    return bank;
}

void sp_mic_window_slot_cancel_fill(int bank)
{
    if (!bank_has_state(bank, BANK_FILLING)) {
        count_slot_event(&s_diag.ownership_violations);
        return;
    }
    s_bank_state[bank] = BANK_FREE;
    count_slot_event(&s_diag.unpublished_fills);
}

bool sp_mic_window_slot_publish(const sp_mic_window_slot_pending_t *in)
{
    if (in == NULL || !bank_has_state(in->buf_idx, BANK_FILLING)) {
        count_slot_event(&s_diag.ownership_violations);
        return false;
    }
    if (s_pending.pending && replace_pending() < 0) {
        return false;
    }
    s_pending = *in;
    s_pending.pending = true;
    s_bank_state[in->buf_idx] = BANK_PENDING;
    s_preferred_bank = (uint8_t)(in->buf_idx ^ 1);
    count_slot_event(&s_diag.published);
    return true;
}

bool sp_mic_window_slot_take(sp_mic_window_slot_pending_t *out, bool discard)
{
    if (!s_pending.pending) {
        return false;
    }
    if (discard) {
        return retire_pending() >= 0;
    }
    if (s_bank_state[0] == BANK_LEASED || s_bank_state[1] == BANK_LEASED) {
        return false;
    }
    /* Never recycle a token, including across pending reset. */
    if (out == NULL || s_last_lease == UINT64_MAX
        || !bank_has_state(s_pending.buf_idx, BANK_PENDING)) {
        count_slot_event(&s_diag.ownership_violations);
        return false;
    }
    *out = s_pending;
    s_pending.pending = false;
    s_bank_state[out->buf_idx] = BANK_LEASED;
    out->lease_token = ++s_last_lease;
    out->pending_overwrites_at_take = s_overwrite_count;
    return true;
}

bool sp_mic_window_slot_release(uint64_t token)
{
    if (token != 0u && token == s_last_lease) {
        for (int bank = 0; bank < 2; ++bank) {
            if (bank_has_state(bank, BANK_LEASED)) {
                s_bank_state[bank] = BANK_FREE;
                return true;
            }
        }
    }
    count_slot_event(&s_diag.invalid_releases);
    return false;
}

void sp_mic_window_slot_note_projection(uint64_t token, bool legacy, bool projected)
{
    if (!projected) {
        count_slot_event(&s_diag.projection_failed);
        (void)sp_mic_window_slot_release(token);
    } else if (legacy) {
        count_slot_event(&s_diag.legacy_takes);
    }
}

void sp_mic_window_slot_note_legacy_generation_mismatch(void)
{
    count_slot_event(&s_diag.legacy_generation_mismatches);
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
    if (banks[0][snap->buf_idx] == NULL) {
        return false;
    }
    for (uint8_t ch = 0; ch < SP_MIC_COUNT_MAX; ++ch) {
        if ((snap->pcm_channel_mask & (1u << ch)) != 0u && banks[ch][snap->buf_idx] == NULL) {
            return false;
        }
    }
    return true;
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
    out->lease_token = snap->lease_token;
#if CONFIG_DBMAP_EAR_AE_DIAG
    out->bank_generation = snap->bank_generation;
    out->bank_index = (uint8_t)snap->buf_idx;
#endif
    out->pending_overwrites_at_take = snap->pending_overwrites_at_take;
    out->pcm_channel_mask = snap->pcm_channel_mask;
    out->active_mic_count = snap->live_mic_count;
    for (uint8_t ch = 0u; ch < SP_MIC_COUNT_MAX; ++ch) {
        if ((snap->pcm_channel_mask & (uint8_t)(1u << ch)) == 0u) {
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
    out->collection_start_mono_us = snap->collection_start_mono_us;
    out->nominal_sample_rate_hz = sample_rate_hz;
    out->incomplete_stereo_frames = snap->incomplete_stereo_frames;
    out->software_missing_frames = snap->software_missing_frames;
    out->carried_frames = snap->carried_frames;
    out->span_end = snap->span_end;
    uint8_t live = snap->live_mic_count;
    if (live < 1u || live > SP_MIC_COUNT_MAX) {
        live = 1u;
    }
    (void)sp_mic_window_job_set_channels_from_views(
        out, live, sample_rate_hz, snap->views
    );
    return true;
}
