#include "../sp_mic_window_job_slot.h"
/**
 * Host tests: coherent PCM + channel window job (race-free pairing).
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -g \
 *     -I firmware/platform/mic/include \
 *     -I firmware/platform/node_common/include \
 *     firmware/platform/mic/sp_mic_window_job.c \
 *     firmware/platform/mic/ear_mic_channel_api.c \
 *     firmware/platform/mic/test_host/test_sp_mic_window_job.c \
 *     -lm -o /tmp/test_sp_mic_window_job && /tmp/test_sp_mic_window_job
 */
#include "sp_mic_window_job.h"
#include "sp_mic_diagnostics.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void fill_view(
    sp_mic_window_view_t *v,
    uint32_t seq,
    int64_t t_us,
    float rms,
    float peak,
    uint32_t sample_count
)
{
    memset(v, 0, sizeof(*v));
    v->window_seq = seq;
    v->measured_at_us = t_us;
    v->window_ms = 1000;
    v->rms_dbfs = rms;
    v->peak_dbfs = peak;
    v->sample_count = sample_count;
    v->ms_since_i2s_enable = 10000;
    strncpy(v->channel_presence, "active", sizeof(v->channel_presence) - 1);
    strncpy(v->transport_status, "ok", sizeof(v->transport_status) - 1);
    strncpy(v->microphone_status, "signal_present", sizeof(v->microphone_status) - 1);
}

static void init_pcm(sp_mic_window_job_t *job, uint32_t seq, int64_t t_us)
{
    memset(job, 0, sizeof(*job));
    job->pcm.window_seq = seq;
    job->pcm.window_end_mono_us = t_us;
    job->pcm.window_ms = 1000;
    job->pcm.sample_count = 16000;
    job->pcm.full_scale_bits = 24;
}

/** NVS mic_count=1: only channel 0 is active; mic1 stays unmasked. */
static void test_mono_mic_count_one(void)
{
    sp_mic_window_job_t job;
    init_pcm(&job, 3u, 3000000);
    sp_mic_window_view_t views[2];
    fill_view(&views[0], 3u, 3000000, -42.0f, -36.0f, 16000);
    fill_view(&views[1], 3u, 3000000, -48.0f, -42.0f, 16000);
    EXPECT(sp_mic_window_job_set_channels_from_views(&job, 1u, 16000u, views) == SP_MIC_API_OK);
    EXPECT(sp_mic_window_job_channels_coherent(&job));
    EXPECT((job.channel_mask & 0x01u) != 0u);
    EXPECT((job.channel_mask & 0x02u) == 0u);
    EXPECT(job.channels[0].window_seq == 3u);
    EXPECT(job.channels[1].valid == false);
}

/** Case 1: PCM N pairs with Mic0 N and Mic1 N when bundled (NVS mic_count=2). */
static void test_pair_same_window(void)
{
    sp_mic_window_job_t job;
    init_pcm(&job, 7u, 7000000);
    sp_mic_window_view_t views[2];
    fill_view(&views[0], 7u, 7000000, -40.0f, -34.0f, 16000);
    fill_view(&views[1], 7u, 7000000, -46.0f, -40.0f, 16000);
    EXPECT(sp_mic_window_job_set_channels_from_views(&job, 2u, 16000u, views) == SP_MIC_API_OK);
    EXPECT(sp_mic_window_job_channels_coherent(&job));
    EXPECT((job.channel_mask & 0x03u) == 0x03u);
    EXPECT(job.channels[0].window_seq == 7u);
    EXPECT(job.channels[1].window_seq == 7u);
    EXPECT(fabsf(job.channels[0].rms_dbfs - (-40.0f)) < 0.01f);
    EXPECT(fabsf(job.channels[1].rms_dbfs - (-46.0f)) < 0.01f);
}

/**
 * Case 2: Publication ordering — bundled job keeps pair even if a separate
 * "latest" snapshot is still N-1 (simulates old race). Consumer must use the
 * bundle, not the delayed latest.
 */
static void test_bundle_survives_stale_latest(void)
{
    sp_mic_window_job_t bundled;
    init_pcm(&bundled, 8u, 8000000);
    sp_mic_window_view_t views_n[2];
    fill_view(&views_n[0], 8u, 8000000, -41.0f, -35.0f, 16000);
    fill_view(&views_n[1], 8u, 8000000, -47.0f, -41.0f, 16000);
    EXPECT(
        sp_mic_window_job_set_channels_from_views(&bundled, 2u, 16000u, views_n)
        == SP_MIC_API_OK
    );
    EXPECT(sp_mic_window_job_channels_coherent(&bundled));

    /* Stale s_latest stand-in (N-1) — must NOT be used for pairing. */
    sp_mic_window_view_t stale[2];
    fill_view(&stale[0], 7u, 7000000, -90.0f, -84.0f, 16000);
    fill_view(&stale[1], 7u, 7000000, -91.0f, -85.0f, 16000);
    EXPECT(stale[0].window_seq != bundled.pcm.window_seq);
    /* Bundle still has valid pair for N. */
    EXPECT((bundled.channel_mask & 0x03u) == 0x03u);
    EXPECT(bundled.channels[0].window_seq == bundled.pcm.window_seq);
}

/** Case 3: Newer snapshot N+1 must not pair with PCM N. */
static void test_reject_newer_snapshot(void)
{
    sp_mic_window_job_t job;
    init_pcm(&job, 9u, 9000000);
    sp_mic_window_view_t views[2];
    fill_view(&views[0], 9u, 9000000, -40.0f, -34.0f, 16000);
    fill_view(&views[1], 10u, 10000000, -46.0f, -40.0f, 16000); /* N+1 */
    EXPECT(sp_mic_window_job_set_channels_from_views(&job, 2u, 16000u, views) == SP_MIC_API_OK);
    EXPECT(!sp_mic_window_job_channels_coherent(&job));
}

/** Case 4: Missing Mic1 is not a zero-valued channel. */
static void test_missing_mic1_not_zero(void)
{
    sp_mic_window_job_t job;
    init_pcm(&job, 11u, 11000000);
    sp_mic_window_view_t views[2];
    fill_view(&views[0], 11u, 11000000, -40.0f, -34.0f, 16000);
    memset(&views[1], 0, sizeof(views[1])); /* unpublished / missing */
    EXPECT(sp_mic_window_job_set_channels_from_views(&job, 2u, 16000u, views) == SP_MIC_API_OK);
    EXPECT((job.channel_mask & 0x01u) != 0u);
    EXPECT((job.channel_mask & 0x02u) == 0u);
    EXPECT(job.channels[1].valid == false);
    EXPECT(job.channels[1].sample_count == 0u);
    /* Mask bit clear ⇒ AE must not invent 0 dBFS / silence floor as Mic1. */
    EXPECT(sp_mic_window_job_channels_coherent(&job)); /* only masked chs checked */
}

/** Case 5: Genuinely invalid / empty measurement left out of mask. */
static void test_invalid_measurement_unmasked(void)
{
    sp_mic_window_job_t job;
    init_pcm(&job, 12u, 12000000);
    sp_mic_window_view_t views[2];
    fill_view(&views[0], 12u, 12000000, -40.0f, -34.0f, 0); /* no samples but timed */
    views[0].sample_count = 0;
    fill_view(&views[1], 12u, 12000000, -46.0f, -40.0f, 16000);
    EXPECT(sp_mic_window_job_set_channels_from_views(&job, 2u, 16000u, views) == SP_MIC_API_OK);
    EXPECT((job.channel_mask & 0x03u) == 0x03u); /* both timed */
    EXPECT(job.channels[0].sample_count == 0u);
    EXPECT(job.channels[1].sample_count > 0u);
}

static void test_capture_spans(void)
{
    static int32_t left[16176] = {[0] = 123, [16175] = -123};
    static int32_t right[16176] = {[0] = 456, [16175] = -456};
    int32_t *banks[SP_MIC_COUNT_MAX][2] = {{left, NULL}, {right, NULL}};
    const uint32_t captured[] = {16000u, 15360u, 17152u};
    const uint32_t retained[] = {16000u, 15360u, 16176u};
    const uint32_t discarded[] = {0u, 0u, 976u};
    for (uint8_t live = 1u; live <= 2u; ++live) {
        for (size_t span = 0; span < sizeof(captured) / sizeof(captured[0]); ++span) {
            const bool fault = live == 2u && span == 2u;
            sp_mic_window_slot_pending_t pending = {
                .live_mic_count = live,
                .pcm_channel_mask = (uint8_t)((1u << live) - 1u),
                .window_seq = 12u,
                .window_ms = 1000u,
                .collection_start_mono_us = 10990000,
                .window_end_mono_us = 12000000,
                .incomplete_stereo_frames = fault ? 1u : 0u,
                .carried_frames = 8u,
                .software_missing_frames = fault ? 1u : 0u,
                .span_end = fault ? SP_MIC_SPAN_STALLED : SP_MIC_SPAN_COMPLETE,
                .full_scale_bits = 24,
            };
            for (uint8_t ch = 0; ch < live; ++ch) {
                fill_view(&pending.views[ch], 12u, 12000000, -40.0f, -30.0f, captured[span]);
                pending.views[ch].read_errors = fault ? 1u : 0u;
                pending.views[ch].timeout_reads = fault ? 2u : 0u;
                pending.views[ch].partial_reads = fault ? 3u : 0u;
                pending.sample_count_ch[ch] = retained[span];
            }
            sp_mic_window_job_t job;
            EXPECT(sp_mic_window_slot_project_window(&pending, banks, 16000u, &job));
            EXPECT(sp_mic_window_job_channels_coherent(&job));
            EXPECT(job.collection_start_mono_us == 10990000);
            EXPECT(job.pcm.window_end_mono_us == 12000000);
            EXPECT(job.pcm.window_ms == 1000u);
            EXPECT(job.nominal_sample_rate_hz == 16000u);
            EXPECT(job.incomplete_stereo_frames == (fault ? 1u : 0u));
            EXPECT(job.carried_frames == 8u);
            EXPECT(job.software_missing_frames == (fault ? 1u : 0u));
            EXPECT(job.span_end == (fault ? SP_MIC_SPAN_STALLED : SP_MIC_SPAN_COMPLETE));
            EXPECT(job.pcm.samples == left);
            EXPECT(job.pcm.sample_count == retained[span]);
            for (uint8_t ch = 0; ch < live; ++ch) {
                uint32_t loss = UINT32_MAX;
                EXPECT(job.channels[ch].sample_count == captured[span]);
                EXPECT(job.channels[ch].read_errors == (fault ? 1u : 0u));
                EXPECT(job.channels[ch].timeout_reads == (fault ? 2u : 0u));
                EXPECT(job.channels[ch].partial_reads == (fault ? 3u : 0u));
                EXPECT(job.pcm.sample_count_ch[ch] == retained[span]);
                EXPECT(job.pcm.samples_ch[ch] == banks[ch][0]);
                EXPECT(job.pcm.samples_ch[ch][0] == (ch == 0u ? 123 : 456));
                EXPECT(sp_mic_window_job_cap_discarded(&job, ch, &loss));
                EXPECT(loss == discarded[span]);
            }
            uint32_t unavailable = UINT32_MAX;
            EXPECT(!sp_mic_window_job_cap_discarded(&job, live, &unavailable));
            EXPECT(unavailable == UINT32_MAX);
        }
    }
    EXPECT(left[16175] == -123);
    EXPECT(right[16175] == -456);
}

static int publish_span(
    uint32_t sequence, uint8_t live, int32_t (*storage)[2], int32_t marker
)
{
    const int bank = sp_mic_window_slot_begin_fill();
    if (bank < 0) {
        return -1;
    }
    sp_mic_window_slot_pending_t job = {
        .buf_idx = bank,
        .window_seq = sequence,
        .live_mic_count = live,
        .pcm_channel_mask = (uint8_t)((1u << live) - 1u),
    };
    for (uint8_t ch = 0; ch < live; ++ch) {
        storage[ch][bank] = marker + ch;
        job.sample_count_ch[ch] = 1u;
    }
    EXPECT(sp_mic_window_slot_publish(&job));
    return bank;
}

static void test_slot_ownership(void)
{
    int32_t storage[SP_MIC_COUNT_MAX][2] = {0};
    int32_t *banks[SP_MIC_COUNT_MAX][2];
    for (uint8_t ch = 0; ch < SP_MIC_COUNT_MAX; ++ch) {
        for (int bank = 0; bank < 2; ++bank) {
            banks[ch][bank] = &storage[ch][bank];
        }
    }
    sp_mic_window_slot_reset_pending();
    sp_mic_window_diag_t before, after;
    sp_mic_window_slot_get_diag(&before);
    for (uint8_t live = 1; live <= SP_MIC_COUNT_MAX; ++live) {
        const int bank = publish_span(1u, live, storage, 100);
        EXPECT(bank >= 0);
        if (bank < 0) {
            return;
        }
        sp_mic_window_slot_pending_t held, taken;
        EXPECT(sp_mic_window_slot_take(&held, false));
        sp_mic_pcm_job_t pcm;
        EXPECT(sp_mic_window_slot_project_pcm(&held, banks, &pcm));
        EXPECT(pcm.pcm_channel_mask == (1u << live) - 1u);
        const uint64_t original = pcm.lease_token;
        const uint32_t overwrites = held.pending_overwrites_at_take;

        EXPECT(publish_span(2u, live, storage, 200) == (bank ^ 1));
        EXPECT(!sp_mic_window_slot_take(&taken, false)); /* pending stays intact */
        EXPECT(publish_span(3u, live, storage, 300) == (bank ^ 1));
        EXPECT(held.pending_overwrites_at_take == overwrites);
        for (uint8_t ch = 0; ch < live; ++ch) {
            EXPECT(pcm.samples_ch[ch] == banks[ch][bank]);
            EXPECT(*pcm.samples_ch[ch] == 100 + ch);
        }
        EXPECT(sp_mic_window_slot_release(original));
        EXPECT(!sp_mic_window_slot_release(original));
        EXPECT(sp_mic_window_slot_take(&taken, false));
        EXPECT(taken.window_seq == 3u);
        EXPECT(taken.pending_overwrites_at_take == overwrites + 1u);
        EXPECT(!sp_mic_window_slot_release(original));

        /* One leased plus one filling leaves no writable bank. */
        const int filling = sp_mic_window_slot_begin_fill();
        EXPECT(filling == bank);
        EXPECT(sp_mic_window_slot_begin_fill() == -1);
        sp_mic_window_slot_reset_pending();
        EXPECT(sp_mic_window_slot_release(taken.lease_token));
        sp_mic_window_slot_cancel_fill(filling);

        EXPECT(publish_span(4u, live, storage, 400) >= 0);
        EXPECT(sp_mic_window_slot_take(NULL, true));
        EXPECT(!sp_mic_window_slot_take(&taken, false));
    }
    sp_mic_window_slot_get_diag(&after);
    EXPECT(after.published == before.published + 4u * SP_MIC_COUNT_MAX);
    EXPECT(after.pending_retired == before.pending_retired + SP_MIC_COUNT_MAX);
    EXPECT(after.overwrites == before.overwrites + SP_MIC_COUNT_MAX);
    EXPECT(after.no_bank_drops == before.no_bank_drops + SP_MIC_COUNT_MAX);
    EXPECT(after.unpublished_fills == before.unpublished_fills + SP_MIC_COUNT_MAX);
    EXPECT(after.invalid_releases == before.invalid_releases + 2u * SP_MIC_COUNT_MAX);
    EXPECT(after.ownership_violations == before.ownership_violations);
}

static void test_projection_and_reset(void)
{
    int32_t storage[SP_MIC_COUNT_MAX][2] = {0};
    int32_t *banks[SP_MIC_COUNT_MAX][2] = {0};
    sp_mic_window_diag_t before, after;
    sp_mic_window_slot_get_diag(&before);
    const int bank = publish_span(10u, 2u, storage, 100);
    EXPECT(bank >= 0);
    if (bank < 0) {
        return;
    }
    sp_mic_window_slot_pending_t held;
    EXPECT(sp_mic_window_slot_take(&held, false));

    EXPECT(publish_span(11u, 2u, storage, 200) == (bank ^ 1));
    sp_mic_window_slot_reset_pending();
    EXPECT(storage[0][bank] == 100 && storage[1][bank] == 101);
    EXPECT(publish_span(12u, 2u, storage, 300) == (bank ^ 1));
    /* Retire unread pending to start a fill that ultimately does not publish. */
    const int unpublished = sp_mic_window_slot_begin_fill();
    EXPECT(unpublished == (bank ^ 1));
    sp_mic_window_slot_cancel_fill(unpublished);
    EXPECT(sp_mic_window_slot_release(held.lease_token));
    EXPECT(publish_span(13u, 2u, storage, 400) >= 0);
    EXPECT(sp_mic_window_slot_take(&held, false));

    /* A missing advertised stereo channel fails the whole projection. */
    banks[0][held.buf_idx] = &storage[0][held.buf_idx];
    sp_mic_window_job_t projected;
    EXPECT(!sp_mic_window_slot_project_window(&held, banks, 16000u, &projected));
    sp_mic_window_slot_note_projection(held.lease_token, false, false);
    EXPECT(publish_span(14u, 2u, storage, 500) >= 0);
    EXPECT(sp_mic_window_slot_take(&held, false));
    EXPECT(held.window_seq == 14u);
    EXPECT(sp_mic_window_slot_release(held.lease_token));
    sp_mic_window_slot_get_diag(&after);
    EXPECT(after.reset_discards == before.reset_discards + 1u);
    EXPECT(after.pending_retired == before.pending_retired + 1u);
    EXPECT(after.unpublished_fills == before.unpublished_fills + 1u);
    EXPECT(after.projection_failed == before.projection_failed + 1u);
    EXPECT(after.invalid_releases == before.invalid_releases);
    EXPECT(after.ownership_violations == before.ownership_violations);
}

static void test_din_transport_totals(void)
{
    sp_mic_din_diag_t totals[2] = {0};
    const sp_mic_din_diag_t din0 = {
        .read_errors = 1, .timeout_reads = 2, .empty_reads = 3,
        .partial_reads = 4, .incomplete_stereo = 5, .capped_windows = 1,
    };
    const sp_mic_din_diag_t din1 = {.partial_reads = 7, .incomplete_stereo = 1};

    /* Each window contributes once per DIN, regardless of its two channels. */
    sp_mic_din_diag_add(&totals[0], &din0);
    sp_mic_din_diag_add(&totals[1], &din1);
    sp_mic_din_diag_add(&totals[0], &din0);
    EXPECT(totals[0].read_errors == 2u);
    EXPECT(totals[0].timeout_reads == 4u);
    EXPECT(totals[0].empty_reads == 6u);
    EXPECT(totals[0].partial_reads == 8u);
    EXPECT(totals[0].incomplete_stereo == 10u);
    EXPECT(totals[0].capped_windows == 2u);
    EXPECT(totals[1].partial_reads == 7u);
    EXPECT(totals[1].incomplete_stereo == 1u);
    EXPECT(totals[1].read_errors == 0u);
}

int main(void)
{
    test_capture_spans();
    test_din_transport_totals();
    test_slot_ownership();
    test_projection_and_reset();
    test_mono_mic_count_one();
    test_pair_same_window();
    test_bundle_survives_stale_latest();
    test_reject_newer_snapshot();
    test_missing_mic1_not_zero();
    test_invalid_measurement_unmasked();
    if (g_failures != 0) {
        fprintf(stderr, "test_sp_mic_window_job: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_sp_mic_window_job: ok\n");
    return 0;
}
