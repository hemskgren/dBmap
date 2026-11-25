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
    const int buf = host_mic_begin_fill();
    if (buf < 0) {
        return -1;
    }
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
    EXPECT(older >= 0);
    EXPECT(newer == (older ^ 1));
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
    const int newer = publish_job(6u, 6000000, -28.0f, 250000);
    EXPECT(newer >= 0);
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
    EXPECT(host_mic_last_pcm() == host_mic_bank(0u, newer));
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
    const ae_signature_t first = g_sig;
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

    /* 4500000 is not the hole time and not seq*1000000. */
    sp_mic_window_slot_pending_t meta;
    const uint32_t n = host_mic_pcm_n();
    fill_meta(&meta, 4u, 4500000, -31.0f, n);
    meta.collection_start_mono_us = 3490000;
    meta.views[0].sample_count += 976u;
    fill_pcm(host_mic_bank(0u, host_mic_begin_fill()), n, 250000);
    EXPECT(host_mic_publish(&meta) >= 0);
    EXPECT(ae_device_poll() == ESP_OK);
    ae_device_diag_t after_recover;
    ae_device_get_diag(&after_recover);
    EXPECT(g_sig.window_seq == 4u);
    EXPECT(g_sig.quality == AE_QUALITY_DEGRADED);
    EXPECT(g_sig.flags == first.flags);
    EXPECT(g_sig.rms_dbfs == first.rms_dbfs);
    EXPECT(g_sig.peak_dbfs == first.peak_dbfs);
    for (size_t band = 0; band < sizeof(first.band_dbfs) / sizeof(first.band_dbfs[0]); ++band) {
        EXPECT(g_sig.band_dbfs[band] == first.band_dbfs[band]);
    }
    EXPECT((g_sig.flags & AE_FLAG_L1_MISSING) != 0u);
    EXPECT(after_recover.seq_gap_events == after_hole.seq_gap_events);
    EXPECT(after_recover.gap_sigs_published == after_hole.gap_sigs_published);
    EXPECT(after_recover.pcm_overwrite_events == after_hole.pcm_overwrite_events);
    EXPECT(ae_publish_rejected_count(ae_device_engine()) == 0u);
    ae_engine_t *ae = ae_device_engine();
    ae_signature_t accepted;
    EXPECT(g_hits == 3u);
    EXPECT(ae_get_latest_signature(ae, &accepted));
    EXPECT(accepted.window_seq == 4u);
    EXPECT(accepted.window_end_mono_us == 4500000);
    EXPECT(g_sig.window_end_mono_us == 4500000);
    EXPECT(ae_last_window_end_mono_us(ae) == 4500000);
    stop_ae();
}

static void test_signature_rejection_and_ae_epoch(void)
{
    host_mic_begin(true, 16000u);
    EXPECT(ae_device_start() == ESP_OK);
    bind_listener();
    EXPECT(publish_job(1u, 1000000, -31.0f, 250000) >= 0);
    EXPECT(ae_device_poll() == ESP_OK);
    EXPECT(publish_job(1u, 2000000, -31.0f, 250000) >= 0);
    EXPECT(ae_device_poll() == ESP_OK);

    ae_device_diag_t before;
    ae_device_get_diag(&before);
    EXPECT(before.pcm_jobs_taken == 2u);
    EXPECT(before.sig_published == 2u);
    EXPECT(before.sig_accepted == 1u);
    EXPECT(before.sig_build_failed == 0u);
    EXPECT(ae_publish_rejected_count(ae_device_engine()) == 1u);
    EXPECT(g_hits == 1u);

    stop_ae();
    EXPECT(ae_device_start() == ESP_OK);
    ae_device_diag_t after;
    ae_device_get_diag(&after);
    EXPECT(after.epoch == before.epoch + 1u);
    EXPECT(after.pcm_jobs_taken == 0u);
    EXPECT(after.sig_accepted == 0u);
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
    EXPECT(sp_mic_release_pcm_job(pcm.lease_token));
    sp_mic_window_job_t discarded;
    EXPECT(!sp_mic_take_window_job(&discarded));

    EXPECT(publish_job(11u, 11000000, -31.0f, 250000) >= 0);
    sp_mic_window_job_t win;
    EXPECT(sp_mic_take_window_job(&win));
    EXPECT(win.pcm.window_seq == 11u);
    EXPECT((win.channel_mask & 0x01u) != 0u);
    EXPECT(win.channels[0].window_seq == 11u);
    EXPECT(win.channels[0].measured_at_us == 11000000);
    EXPECT(sp_mic_release_pcm_job(win.pcm.lease_token));
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
    EXPECT(!sp_mic_take_pcm_job(&pcm));
    EXPECT(sp_mic_release_pcm_job(win.pcm.lease_token));
    EXPECT(sp_mic_take_pcm_job(&pcm));
    EXPECT(pcm.window_seq == 15u);
    EXPECT(sp_mic_release_pcm_job(pcm.lease_token));
}

static void replace_pending_during_analysis(void)
{
    const int32_t *const leased = host_mic_last_pcm();
    EXPECT(leased != NULL && leased[0] == 250000);
    EXPECT(publish_job(2u, 2000000, -38.0f, 170000) >= 0);
    EXPECT(publish_job(3u, 3000000, -37.0f, 180000) >= 0);
    EXPECT(leased[0] == 250000 && leased[1] == -250000);
}

static void test_retirement_gap_is_on_successor(void)
{
    host_mic_begin(true, 16000u);
    EXPECT(ae_device_start() == ESP_OK);
    bind_listener();
    EXPECT(publish_job(1u, 1000000, -31.0f, 250000) >= 0);
    host_mic_on_second_ready_check(replace_pending_during_analysis);
    EXPECT(ae_device_poll() == ESP_OK);
    EXPECT(g_sig.window_seq == 1u && g_sig.quality == AE_QUALITY_DEGRADED);
    EXPECT(ae_device_poll() == ESP_OK);
    EXPECT(g_sig.window_seq == 3u && g_sig.quality == AE_QUALITY_GAP);
    ae_device_diag_t diag;
    ae_device_get_diag(&diag);
    EXPECT(diag.pcm_overwrite_events == 1u);
    EXPECT(diag.seq_gap_events == 1u);
    EXPECT(diag.gap_sigs_published == 1u);
    stop_ae();
}

static void fail_analysis(void)
{
    host_mic_set_ready(false);
}

static void test_consumer_errors_release(void)
{
    host_mic_begin(true, 16000u);
    EXPECT(ae_device_start() == ESP_OK);
    EXPECT(publish_job(1u, 1000000, -31.0f, 250000) >= 0);
    host_mic_on_second_ready_check(fail_analysis);
    EXPECT(ae_device_poll() == ESP_ERR_INVALID_STATE);
    host_mic_set_ready(true);

    sp_mic_window_slot_pending_t empty;
    fill_meta(&empty, 2u, 2000000, -31.0f, 0);
    EXPECT(host_mic_begin_fill() >= 0);
    EXPECT(host_mic_publish(&empty) >= 0);
    EXPECT(ae_device_poll() == ESP_ERR_INVALID_STATE);
    EXPECT(publish_job(3u, 3000000, -31.0f, 250000) >= 0);
    EXPECT(ae_device_poll() == ESP_OK);
    stop_ae();

    EXPECT(publish_job(4u, 4000000, -31.0f, 250000) >= 0);
    host_mic_on_second_ready_check(fail_analysis);
    EXPECT(dbmap_ear_mic_l0_poll() == ESP_ERR_INVALID_STATE);
    host_mic_set_ready(true);
    EXPECT(publish_job(5u, 5000000, -31.0f, 250000) >= 0);
    EXPECT(dbmap_ear_mic_l0_poll() == ESP_OK);
    sp_mic_window_diag_t diag;
    sp_mic_window_slot_get_diag(&diag);
    EXPECT(diag.invalid_releases == 0u);
    EXPECT(diag.ownership_violations == 0u);
    EXPECT(diag.legacy_generation_mismatches == 0u);
}

#if CONFIG_DBMAP_EAR_AE_DIAG
#include "../ae_pcm_diagnostics.h"
#include "sp_mic_bank_diag.h"

static void test_pcm_bank_diagnostics(void)
{
    sp_mic_bank_generation_t banks[2] = {UINT32_MAX - 1u, 0u};
    ae_pcm_diagnostics_t diag = {0};
    const uint32_t expected = sp_mic_bank_write_start(&banks[0]);
    EXPECT(expected == UINT32_MAX);
    const uint32_t before = sp_mic_bank_generation(&banks[0]);
    EXPECT(sp_mic_bank_write_start(&banks[1]) == 1u);
    ae_pcm_diag_record_check(&diag, expected, before, sp_mic_bank_generation(&banks[0]));
    EXPECT(diag.checked == 1u);
    EXPECT(diag.stale_before_access == 0u);
    EXPECT(diag.reuse_during_access == 0u);

    /* A fill that never publishes still invalidates a borrow across rollover. */
    EXPECT(sp_mic_bank_write_start(&banks[0]) == 0u);
    const uint32_t stale = sp_mic_bank_generation(&banks[0]);
    ae_pcm_diag_record_check(&diag, expected, stale, stale);
    EXPECT(diag.checked == 2u);
    EXPECT(diag.stale_before_access == 1u);
    EXPECT(diag.reuse_during_access == 0u);

    const uint32_t current = sp_mic_bank_generation(&banks[0]);
    EXPECT(sp_mic_bank_write_start(&banks[0]) == 1u);
    ae_pcm_diag_record_check(&diag, current, current, sp_mic_bank_generation(&banks[0]));
    EXPECT(diag.checked == 3u);
    EXPECT(diag.stale_before_access == 1u);
    EXPECT(diag.reuse_during_access == 1u);
}

static void test_pcm_span_and_age_diagnostics(void)
{
    ae_pcm_diagnostics_t diag = {0};
    sp_mic_window_slot_pending_t pending;
    sp_mic_window_job_t job;
    int32_t left = 0;
    int32_t right = 0;
    int32_t *banks[SP_MIC_COUNT_MAX][2] = {{&left, NULL}, {&right, NULL}};
    fill_meta(&pending, 7u, 7000000, -35.0f, 16000u);
    pending.bank_generation = UINT32_MAX;
    pending.live_mic_count = 2u;
    pending.pcm_channel_mask = 3u;
    pending.sample_count_ch[1] = 16000u;
    pending.views[1] = pending.views[0];
    pending.views[0].sample_count += 976u;
    pending.views[1].sample_count += 100u;
    EXPECT(sp_mic_window_slot_project_window(&pending, banks, 16000u, &job));
    EXPECT(job.pcm.bank_index == 0u && job.pcm.bank_generation == UINT32_MAX);
    ae_pcm_diag_record_spans(&diag, &job);
    EXPECT(diag.comparable_mask == 3u);
    EXPECT(diag.channels[0].captured == 16976u);
    EXPECT(diag.channels[0].retained == 16000u);
    EXPECT(diag.channels[0].discarded == 976u);

    pending.views[0].sample_count = 16128u;
    pending.views[1].sample_count = 16000u;
    EXPECT(sp_mic_window_slot_project_window(&pending, banks, 16000u, &job));
    ae_pcm_diag_record_spans(&diag, &job);
    EXPECT(diag.channels[0].discard_total == 1104u);
    EXPECT(diag.channels[0].discard_max == 976u);
    EXPECT(diag.channels[1].discard_total == 100u);
    EXPECT(diag.channels[1].discard_max == 100u);

    sp_mic_time_diag_record(&diag.queue_age, job.pcm.window_end_mono_us, 7030000);
    sp_mic_time_diag_record(&diag.queue_age, job.pcm.window_end_mono_us, 7010000);
    sp_mic_time_diag_record(&diag.queue_age, job.pcm.window_end_mono_us, 6999999);
    sp_mic_time_diag_record(&diag.processing_age, job.pcm.window_end_mono_us, 7400000);
    EXPECT(diag.queue_age.count == 2u);
    EXPECT(diag.queue_age.min_us == 10000u);
    EXPECT(diag.queue_age.max_us == 30000u);
    EXPECT(diag.queue_age.last_us == 10000u);
    EXPECT(diag.processing_age.count == 1u);
    EXPECT(diag.processing_age.last_us == 400000u);
}
#endif

int main(void)
{
#if CONFIG_DBMAP_EAR_AE_DIAG
    test_pcm_bank_diagnostics();
    test_pcm_span_and_age_diagnostics();
#endif
    EXPECT(dbmap_ear_acoustic_register() == ESP_OK);
    test_inactive_not_ready_empty();
    test_start_baseline_and_single_delivery();
    test_pending_overwrite();
    test_seq_hole_and_recovery();
    test_signature_rejection_and_ae_epoch();
    test_both_take_apis();
    test_retirement_gap_is_on_successor();
    test_consumer_errors_release();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ae_device_intake: ok\n");
    return 0;
}
