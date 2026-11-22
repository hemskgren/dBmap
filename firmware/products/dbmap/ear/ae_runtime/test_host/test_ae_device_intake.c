/**
 * Package 5: real ae_device_poll and acoustic analyze over the production
 * window-job slot. L1/L2 are off, so an ordinary mono signature is DEGRADED.
 *
 * Host link (repo root), shim directory first:
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -g \
 *     -DCONFIG_DBMAP_EAR_AUDIO_ENGINE=1 -DCONFIG_SP_AUDIO_L0=1 \
 *     -I firmware/products/dbmap/ear/ae_runtime/test_host/host_shim \
 *     -I firmware/products/dbmap/ear/ae_runtime/test_host \
 *     -I firmware/products/dbmap/ear/ae_runtime/include \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     -I firmware/platform/audio/include \
 *     -I firmware/platform/mic/include \
 *     -I firmware/platform/mic \
 *     -I firmware/platform/node_common/include \
 *     firmware/platform/audio/sp_acoustic_l0.c \
 *     firmware/platform/audio/sp_audio_analyze.c \
 *     firmware/platform/mic/sp_mic_window_job.c \
 *     firmware/platform/mic/ear_mic_channel_api.c \
 *     firmware/platform/mic/audio_metrics.c \
 *     firmware/products/dbmap/ear/ear_acoustic/ear_acoustic_runtime.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_align.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_stereo.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_gap.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_history.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_time.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_engine.c \
 *     firmware/products/dbmap/ear/ae_runtime/ae_sig_capture.c \
 *     firmware/products/dbmap/ear/ae_runtime/ae_device.c \
 *     firmware/products/dbmap/ear/ae_runtime/test_host/host_mic_facade.c \
 *     firmware/products/dbmap/ear/ae_runtime/test_host/test_ae_device_intake.c \
 *     -lm
 */
#include "ae_device.h"
#include "ae_engine.h"
#include "dbmap_ear_acoustic.h"
#include "host_mic_facade.h"
#include "sp_mic_pcm.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static uint32_t g_hits;
static ae_signature_t g_sig;

static void on_sig(const ae_signature_t *sig, void *ctx)
{
    (void)ctx;
    g_hits++;
    if (sig != NULL) {
        g_sig = *sig;
    }
}

static void fill_pcm(int32_t *dst, uint32_t n, int32_t amp)
{
    for (uint32_t i = 0u; i < n; ++i) {
        dst[i] = ((i & 1u) == 0u) ? amp : -amp;
    }
}

static void fill_meta(
    sp_mic_window_slot_pending_t *m,
    uint32_t seq,
    int64_t end_us,
    float rms_dbfs,
    uint32_t n
)
{
    memset(m, 0, sizeof(*m));
    m->sample_count_ch[0] = n;
    m->pcm_channel_mask = 0x01u;
    m->window_ms = 1000u;
    m->window_end_mono_us = end_us;
    m->full_scale_bits = 24;
    m->window_seq = seq;
    m->live_mic_count = 1u;
    m->views[0].rms_dbfs = rms_dbfs;
    m->views[0].peak_dbfs = rms_dbfs + 6.0f;
    m->views[0].sample_count = n;
    m->views[0].window_ms = 1000u;
    m->views[0].window_seq = seq;
    m->views[0].measured_at_us = end_us;
    m->views[0].ms_since_i2s_enable = 1000u;
    strncpy(m->views[0].microphone_status, "signal_present",
            sizeof(m->views[0].microphone_status) - 1u);
    strncpy(m->views[0].transport_status, "ok",
            sizeof(m->views[0].transport_status) - 1u);
    strncpy(m->views[0].channel_presence, "active",
            sizeof(m->views[0].channel_presence) - 1u);
}

static int publish_job(uint32_t seq, int64_t end_us, float rms, int32_t amp)
{
    const uint32_t n = host_mic_pcm_n();
    const int buf = host_mic_fill_idx();
    int32_t *bank = host_mic_bank(0u, buf);
    if (amp == 0) {
        memset(bank, 0, (size_t)n * sizeof(*bank));
    } else {
        fill_pcm(bank, n, amp);
    }
    sp_mic_window_slot_pending_t meta;
    fill_meta(&meta, seq, end_us, rms, n);
    return host_mic_publish(&meta);
}

static void bind_listener(void)
{
    g_hits = 0u;
    memset(&g_sig, 0, sizeof(g_sig));
    ae_engine_t *ae = ae_device_engine();
    EXPECT(ae != NULL);
    EXPECT(ae_add_signature_listener(ae, on_sig, NULL));
}

static void stop_ae(void)
{
    ae_device_stop();
}

static void test_inactive_not_ready_empty(void)
{
    host_mic_begin(false, 16000u);
    ae_device_diag_t d0;
    ae_device_get_diag(&d0);
    EXPECT(ae_device_poll() == ESP_ERR_INVALID_STATE);
    ae_device_diag_t d1;
    ae_device_get_diag(&d1);
    EXPECT(memcmp(&d0, &d1, sizeof(d0)) == 0);
    EXPECT(g_hits == 0u);

    EXPECT(ae_device_start() == ESP_OK);
    bind_listener();
    const uint32_t hits = g_hits;
    EXPECT(ae_device_poll() == ESP_OK);
    ae_device_get_diag(&d1);
    EXPECT(d1.no_job == 0u);
    EXPECT(d1.pcm_jobs_taken == 0u);
    EXPECT(d1.analyze_ok == 0u);
    EXPECT(d1.analyze_n == 0u);
    EXPECT(d1.analyze_fail == 0u);
    EXPECT(d1.sig_published == 0u);
    EXPECT(g_hits == hits);
    stop_ae();

    host_mic_begin(true, 16000u);
    EXPECT(ae_device_start() == ESP_OK);
    bind_listener();
    EXPECT(ae_device_poll() == ESP_OK);
    ae_device_diag_t ready_empty;
    ae_device_get_diag(&ready_empty);
    EXPECT(ready_empty.no_job == 1u);
    EXPECT(ready_empty.pcm_jobs_taken == 0u);
    EXPECT(ready_empty.analyze_ok == 0u);
    EXPECT(ready_empty.analyze_n == 0u);
    EXPECT(ready_empty.analyze_fail == 0u);
    EXPECT(ready_empty.sig_published == 0u);
    EXPECT(ready_empty.gap_sigs_published == 0u);
    EXPECT(g_hits == 0u);
    stop_ae();
}

static void test_start_baseline_and_single_delivery(void)
{
    host_mic_begin(true, 16000u);
    const uint32_t ow0 = sp_mic_pcm_pending_overwrite();
    const int older = publish_job(1u, 1000000, -62.0f, 0);
    const int newer = publish_job(2u, 2000000, -31.0f, 250000);
    EXPECT(older == 0);
    EXPECT(newer == 1);
    EXPECT(sp_mic_pcm_pending_overwrite() == ow0 + 1u);
    EXPECT(ae_device_start() == ESP_OK);
    bind_listener();

    EXPECT(ae_device_poll() == ESP_OK);
    ae_device_diag_t d;
    ae_device_get_diag(&d);
    EXPECT(d.pcm_jobs_taken == 1u);
    EXPECT(d.analyze_ok == 1u);
    EXPECT(d.analyze_n == 1u);
    EXPECT(d.analyze_fail == 0u);
    EXPECT(d.sig_published == 1u);
    EXPECT(d.pcm_overwrite_events == 0u);
    EXPECT(d.seq_gap_events == 0u);
    EXPECT(d.gap_sigs_published == 0u);
    EXPECT(d.last_pcm_seq == 2u);
    EXPECT(d.last_pcm_end_us == 2000000);
    EXPECT(d.last_pcm_mask == 0x01u);
    EXPECT(d.last_pcm_n0 == host_mic_pcm_n());
    EXPECT(fabsf(d.last_mic0_rms_dbfs - (-31.0f)) < 0.01f);
    EXPECT(d.mic0_aligned == 1u);
    EXPECT(host_mic_last_pcm() == host_mic_bank(0u, newer));
    EXPECT(g_hits == 1u);
    EXPECT(g_sig.window_seq == 2u);
    EXPECT(g_sig.window_end_mono_us == 2000000);
    EXPECT(g_sig.quality == AE_QUALITY_DEGRADED);
    EXPECT((g_sig.flags & AE_FLAG_L1_MISSING) != 0u);
    EXPECT(g_sig.rms_dbfs > -45.0f);
    ae_engine_t *ae = ae_device_engine();
    EXPECT(ae_last_window_seq(ae) == 2u);
    EXPECT(ae_publish_rejected_count(ae) == 0u);

    ae_device_diag_t before;
    ae_device_get_diag(&before);
    const uint32_t hits = g_hits;
    EXPECT(ae_device_poll() == ESP_OK);
    ae_device_get_diag(&d);
    EXPECT(d.no_job == before.no_job + 1u);
    EXPECT(d.pcm_jobs_taken == before.pcm_jobs_taken);
    EXPECT(d.analyze_ok == before.analyze_ok);
    EXPECT(d.analyze_n == before.analyze_n);
    EXPECT(d.analyze_fail == before.analyze_fail);
    EXPECT(d.sig_published == before.sig_published);
    EXPECT(d.pcm_overwrite_events == before.pcm_overwrite_events);
    EXPECT(d.seq_gap_events == before.seq_gap_events);
    EXPECT(d.gap_sigs_published == before.gap_sigs_published);
    EXPECT(g_hits == hits);
    stop_ae();
}

static void test_pending_overwrite(void)
{
    host_mic_begin(true, 16000u);
    EXPECT(ae_device_start() == ESP_OK);
    bind_listener();
    const uint32_t ow0 = sp_mic_pcm_pending_overwrite();
    ae_device_diag_t d0;
    ae_device_get_diag(&d0);
    EXPECT(publish_job(5u, 5000000, -62.0f, 0) >= 0);
    EXPECT(publish_job(6u, 6000000, -28.0f, 250000) >= 0);
    EXPECT(sp_mic_pcm_pending_overwrite() == ow0 + 1u);
    EXPECT(ae_device_poll() == ESP_OK);
    ae_device_diag_t d;
    ae_device_get_diag(&d);
    EXPECT(d.pcm_overwrite_events == d0.pcm_overwrite_events + 1u);
    EXPECT(d.seq_gap_events == d0.seq_gap_events);
    EXPECT(d.gap_sigs_published == d0.gap_sigs_published + 1u);
    EXPECT(d.pcm_jobs_taken == 1u);
    EXPECT(d.analyze_ok == 1u);
    EXPECT(g_hits == 1u);
    EXPECT(g_sig.window_seq == 6u);
    EXPECT(g_sig.window_end_mono_us == 6000000);
    EXPECT(g_sig.quality == AE_QUALITY_GAP);
    EXPECT(host_mic_last_pcm() == host_mic_bank(0u, 1));
    EXPECT(ae_device_poll() == ESP_OK);
    EXPECT(g_hits == 1u);
    stop_ae();
}

static void test_seq_hole_and_recovery(void)
{
    host_mic_begin(true, 16000u);
    EXPECT(ae_device_start() == ESP_OK);
    bind_listener();
    EXPECT(publish_job(1u, 1000000, -31.0f, 250000) >= 0);
    EXPECT(ae_device_poll() == ESP_OK);
    EXPECT(g_sig.quality == AE_QUALITY_DEGRADED);
    ae_device_diag_t after_first;
    ae_device_get_diag(&after_first);

    EXPECT(publish_job(3u, 3000000, -31.0f, 250000) >= 0);
    EXPECT(ae_device_poll() == ESP_OK);
    ae_device_diag_t after_hole;
    ae_device_get_diag(&after_hole);
    EXPECT(after_hole.pcm_overwrite_events == after_first.pcm_overwrite_events);
    EXPECT(after_hole.seq_gap_events == after_first.seq_gap_events + 1u);
    EXPECT(after_hole.gap_sigs_published == after_first.gap_sigs_published + 1u);
    EXPECT(g_sig.window_seq == 3u);
    EXPECT(g_sig.quality == AE_QUALITY_GAP);

    EXPECT(publish_job(4u, 4000000, -31.0f, 250000) >= 0);
    EXPECT(ae_device_poll() == ESP_OK);
    ae_device_diag_t after_recover;
    ae_device_get_diag(&after_recover);
    EXPECT(g_sig.window_seq == 4u);
    EXPECT(g_sig.quality == AE_QUALITY_DEGRADED);
    EXPECT((g_sig.flags & AE_FLAG_L1_MISSING) != 0u);
    EXPECT(after_recover.seq_gap_events == after_hole.seq_gap_events);
    EXPECT(after_recover.gap_sigs_published == after_hole.gap_sigs_published);
    EXPECT(after_recover.pcm_overwrite_events == after_hole.pcm_overwrite_events);
    EXPECT(ae_publish_rejected_count(ae_device_engine()) == 0u);
    stop_ae();
}

static void test_both_take_apis(void)
{
    host_mic_begin(true, 16000u);
    stop_ae();

    EXPECT(publish_job(10u, 10000000, -31.0f, 250000) >= 0);
    sp_mic_pcm_job_t pcm;
    EXPECT(sp_mic_take_pcm_job(&pcm));
    EXPECT(pcm.window_seq == 10u);
    sp_mic_window_job_t discarded;
    EXPECT(!sp_mic_take_window_job(&discarded));

    EXPECT(publish_job(11u, 11000000, -31.0f, 250000) >= 0);
    sp_mic_window_job_t win;
    EXPECT(sp_mic_take_window_job(&win));
    EXPECT(win.pcm.window_seq == 11u);
    EXPECT((win.channel_mask & 0x01u) != 0u);
    EXPECT(win.channels[0].window_seq == 11u);
    EXPECT(win.channels[0].measured_at_us == 11000000);
    EXPECT(!sp_mic_take_pcm_job(&pcm));

    EXPECT(publish_job(12u, 12000000, -31.0f, 250000) >= 0);
    EXPECT(sp_mic_take_pcm_job(NULL));
    EXPECT(!sp_mic_take_pcm_job(&pcm));
    EXPECT(!sp_mic_take_window_job(&win));

    EXPECT(publish_job(13u, 13000000, -31.0f, 250000) >= 0);
    EXPECT(sp_mic_take_window_job(NULL));
    EXPECT(!sp_mic_take_window_job(&win));
    EXPECT(!sp_mic_take_pcm_job(&pcm));

    EXPECT(publish_job(14u, 14000000, -31.0f, 250000) >= 0);
    EXPECT(sp_mic_take_window_job(&win));
    const uint32_t ow = sp_mic_pcm_pending_overwrite();
    EXPECT(publish_job(15u, 15000000, -31.0f, 250000) >= 0);
    EXPECT(sp_mic_pcm_pending_overwrite() == ow);
    EXPECT(sp_mic_take_pcm_job(&pcm));
    EXPECT(pcm.window_seq == 15u);
}

int main(void)
{
    EXPECT(dbmap_ear_acoustic_register() == ESP_OK);
    test_inactive_not_ready_empty();
    test_start_baseline_and_single_delivery();
    test_pending_overwrite();
    test_seq_hole_and_recovery();
    test_both_take_apis();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ae_device_intake: ok\n");
    return 0;
}
