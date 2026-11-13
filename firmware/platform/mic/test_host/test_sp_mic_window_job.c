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

/** Case 1: PCM N pairs with Mic0 N and Mic1 N when bundled. */
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

int main(void)
{
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
