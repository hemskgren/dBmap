/**
 * Host tests: AE align + signature using coherent window-job channels.
 *
 *   cc -std=c11 -Wall -Wextra -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     -I firmware/platform/mic/include \
 *     -I firmware/platform/node_common/include \
 *     firmware/platform/mic/sp_mic_window_job.c \
 *     firmware/platform/mic/ear_mic_channel_api.c \
 *     firmware/platform/mic/audio_metrics.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_align.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/audio_engine/test_host/test_ae_window_job_align.c \
 *     -lm -o /tmp/test_ae_window_job_align && /tmp/test_ae_window_job_align
 *
 * Note: omit -Werror — host gcc warns on strncpy in ear_mic_channel_api.c
 * (-Wstringop-truncation). Link audio_metrics.c for sp_audio_expected_samples.
 */
#include "ae_align.h"
#include "ae_signature.h"
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
    uint32_t n
)
{
    memset(v, 0, sizeof(*v));
    v->window_seq = seq;
    v->measured_at_us = t_us;
    v->window_ms = 1000;
    v->rms_dbfs = rms;
    v->peak_dbfs = peak;
    v->sample_count = n;
    strncpy(v->microphone_status, "signal_present", sizeof(v->microphone_status) - 1);
    strncpy(v->transport_status, "ok", sizeof(v->transport_status) - 1);
    strncpy(v->channel_presence, "active", sizeof(v->channel_presence) - 1);
}

static void snap_from_ch(const sp_mic_channel_t *ch, bool ok, ae_channel_snap_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!ok || ch == NULL || !ch->valid) {
        return;
    }
    out->present = true;
    out->window_seq = ch->window_seq;
    out->measured_at_us = ch->measured_at_us;
    out->has_samples = ch->sample_count > 0u;
    out->rms_dbfs = ch->rms_dbfs;
    out->peak_dbfs = ch->peak_dbfs;
    out->degraded = false;
}

/** Cases 1–2: bundled N → AE pair; stale latest ignored. */
static void test_ae_pair_from_bundle(void)
{
    sp_mic_window_job_t job;
    memset(&job, 0, sizeof(job));
    job.pcm.window_seq = 5u;
    job.pcm.window_end_mono_us = 5000000;
    job.pcm.window_ms = 1000;
    sp_mic_window_view_t views[2];
    fill_view(&views[0], 5u, 5000000, -40.0f, -34.0f, 16000);
    fill_view(&views[1], 5u, 5000000, -46.0f, -40.0f, 16000);
    EXPECT(sp_mic_window_job_set_channels_from_views(&job, 2u, 16000u, views) == SP_MIC_API_OK);

    ae_align_input_t ain;
    memset(&ain, 0, sizeof(ain));
    ain.pcm_window_seq = job.pcm.window_seq;
    ain.pcm_end_mono_us = job.pcm.window_end_mono_us;
    ain.pcm_seq_valid = true;
    snap_from_ch(&job.channels[0], (job.channel_mask & 1u) != 0u, &ain.mic0);
    snap_from_ch(&job.channels[1], (job.channel_mask & 2u) != 0u, &ain.mic1);

    ae_align_result_t ares;
    ae_align_channels(&ain, &ares);
    EXPECT(ares.pair_aligned);
    EXPECT(fabsf(ares.pair.mic0_rms_dbfs - (-40.0f)) < 0.01f);
    EXPECT(fabsf(ares.pair.mic1_rms_dbfs - (-46.0f)) < 0.01f);
}

/** Case 3: PCM N vs channel N+1 rejected. */
static void test_ae_reject_n_plus_one(void)
{
    ae_align_input_t ain;
    memset(&ain, 0, sizeof(ain));
    ain.pcm_window_seq = 5u;
    ain.pcm_end_mono_us = 5000000;
    ain.pcm_seq_valid = true;
    ain.mic0.present = true;
    ain.mic0.window_seq = 5u;
    ain.mic0.measured_at_us = 5000000;
    ain.mic0.has_samples = true;
    ain.mic0.rms_dbfs = -40.0f;
    ain.mic1.present = true;
    ain.mic1.window_seq = 6u;
    ain.mic1.measured_at_us = 6000000;
    ain.mic1.has_samples = true;
    ain.mic1.rms_dbfs = -46.0f;
    ae_align_result_t ares;
    ae_align_channels(&ain, &ares);
    EXPECT(!ares.pair_aligned);
    EXPECT((ares.mismatch_flags & AE_ALIGN_MISMATCH_MIC1) != 0u);
}

/** Case 4–5: missing Mic1 not zero; mono signature still ok. */
static void test_mono_when_mic1_missing(void)
{
    dbmap_l0_window_t l0;
    memset(&l0, 0, sizeof(l0));
    l0.valid = true;
    l0.window_end_mono_us = 5000000;
    l0.window_ms = 1000;
    l0.rms_dbfs = -40.0f;
    l0.peak_dbfs = -34.0f;
    l0.crest_db = 6.0f;
    l0.band_dbfs[0] = -43.0f;
    l0.band_dbfs[1] = -40.0f;
    l0.band_dbfs[2] = -41.0f;
    l0.band_dbfs[3] = -45.0f;
    l0.hl_valid = true;

    dbmap_l1_window_t l1;
    memset(&l1, 0, sizeof(l1));
    l1.valid = true;
    l1.centroid_hz = 1000.0f;
    l1.bandwidth_hz = 200.0f;
    l1.dominant_hz = 880.0f;
    l1.flatness = 0.3f;

    ae_signature_t sig;
    EXPECT(ae_signature_build(5u, &l0, &l1, NULL, NULL, false, &sig));
    EXPECT(sig.quality == AE_QUALITY_OK); /* L2 missing alone OK */
    EXPECT(!sig.pair_valid);
    EXPECT((sig.flags & AE_FLAG_PAIR_MISSING) != 0u);
    EXPECT((sig.flags & AE_FLAG_L2_MISSING) != 0u);
}

/** Case 6: L1 present / L2 missing with valid pair. */
static void test_l1_l2_with_pair(void)
{
    dbmap_l0_window_t l0;
    memset(&l0, 0, sizeof(l0));
    l0.valid = true;
    l0.window_end_mono_us = 1;
    l0.window_ms = 1000;
    l0.rms_dbfs = -40.0f;
    l0.peak_dbfs = -34.0f;
    l0.crest_db = 6.0f;
    for (int i = 0; i < 4; i++) {
        l0.band_dbfs[i] = -40.0f - (float)i;
    }
    l0.hl_valid = true;

    dbmap_l1_window_t l1;
    memset(&l1, 0, sizeof(l1));
    l1.valid = true;
    l1.dominant_hz = 440.0f;
    l1.flatness = 0.2f;
    l1.centroid_hz = 500.0f;
    l1.bandwidth_hz = 100.0f;

    ae_pair_metrics_t pair = {
        .valid = true,
        .mic0_rms_dbfs = -40.0f,
        .mic1_rms_dbfs = -46.0f,
        .mic0_peak_dbfs = -34.0f,
        .mic1_peak_dbfs = -40.0f,
        .mic_mask = 0x03u,
        .stereo_degraded = false,
    };
    ae_signature_t sig;
    EXPECT(ae_signature_build(1u, &l0, &l1, NULL, &pair, false, &sig));
    EXPECT(sig.quality == AE_QUALITY_OK);
    EXPECT(sig.pair_valid);
    EXPECT(sig.spectral_valid);
}

int main(void)
{
    test_ae_pair_from_bundle();
    test_ae_reject_n_plus_one();
    test_mono_when_mic1_missing();
    test_l1_l2_with_pair();
    if (g_failures != 0) {
        fprintf(stderr, "test_ae_window_job_align: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ae_window_job_align: ok\n");
    return 0;
}
