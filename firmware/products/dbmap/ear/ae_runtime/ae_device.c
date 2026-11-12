#include "ae_device.h"

#include "ae_align.h"
#include "ae_gap.h"
#include "ae_signature.h"
#include "ae_stereo.h"
#include "dbmap_ear_acoustic.h"
#include "sp_mic.h"
#include "sp_mic_pcm.h"
#include "sp_mic_window_job.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include <string.h>

#if defined(CONFIG_DBMAP_EAR_AUDIO_ENGINE) && CONFIG_DBMAP_EAR_AUDIO_ENGINE

static const char *TAG = "ae";

static ae_engine_t s_ae;
static bool s_active;
static ae_device_diag_t s_diag;
static uint32_t s_diag_publish_count;
static uint32_t s_last_overwrite_seen;

static bool channel_degraded(const sp_mic_channel_t *ch)
{
    if (ch == NULL) {
        return true;
    }
    /* Pair may still form; stereo_degraded marks non-signal_present mics. */
    return strcmp(ch->microphone_status, "signal_present") != 0;
}

static void fill_snap(const sp_mic_channel_t *ch, bool api_ok, ae_channel_snap_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!api_ok || ch == NULL) {
        return;
    }
    out->present = ch->valid;
    out->window_seq = ch->window_seq;
    out->measured_at_us = ch->measured_at_us;
    out->has_samples = ch->valid && ch->sample_count > 0u;
    out->rms_dbfs = ch->rms_dbfs;
    out->peak_dbfs = ch->peak_dbfs;
    out->degraded = channel_degraded(ch);
}

static void maybe_diag_log(void)
{
#if defined(CONFIG_DBMAP_EAR_AE_DIAG) && CONFIG_DBMAP_EAR_AE_DIAG
    const uint32_t every = (uint32_t)CONFIG_DBMAP_EAR_AE_DIAG_EVERY_N;
    if (every == 0u || (s_diag_publish_count % every) != 0u) {
        return;
    }
    ESP_LOGI(
        TAG,
        "diag jobs=%lu analyze_ok=%lu sig=%lu m0=%lu m1=%lu pair=%lu "
        "mis_pair=%lu no_job=%lu last_seq=%lu last_q=%u "
        "ow=%lu seq_gap=%lu gap_sig=%lu listeners=%u mis_flags=0x%lx "
        "stereo=%s pcm_mask=0x%x n0=%lu n1=%lu rms0=%.1f rms1=%.1f "
        "st_ok=%lu st_mono=%lu st_mis=%lu st_deg=%lu st_pcm=%lu",
        (unsigned long)s_diag.pcm_jobs_taken,
        (unsigned long)s_diag.analyze_ok,
        (unsigned long)s_diag.sig_published,
        (unsigned long)s_diag.mic0_aligned,
        (unsigned long)s_diag.mic1_aligned,
        (unsigned long)s_diag.pair_aligned,
        (unsigned long)s_diag.mismatch_reject_pair,
        (unsigned long)s_diag.no_job,
        (unsigned long)s_diag.last_pcm_seq,
        (unsigned)s_diag.last_quality,
        (unsigned long)s_diag.pcm_overwrite_events,
        (unsigned long)s_diag.seq_gap_events,
        (unsigned long)s_diag.gap_sigs_published,
        (unsigned)ae_listener_count(&s_ae),
        (unsigned long)s_diag.last_mismatch_flags,
        ae_stereo_state_str((ae_stereo_state_t)s_diag.last_stereo_state),
        (unsigned)s_diag.last_pcm_mask,
        (unsigned long)s_diag.last_pcm_n0,
        (unsigned long)s_diag.last_pcm_n1,
        (double)s_diag.last_mic0_rms_dbfs,
        (double)s_diag.last_mic1_rms_dbfs,
        (unsigned long)s_diag.stereo_ok,
        (unsigned long)s_diag.stereo_mono_fallback,
        (unsigned long)s_diag.stereo_misaligned,
        (unsigned long)s_diag.stereo_degraded,
        (unsigned long)s_diag.stereo_pcm_incomplete
    );
#else
    (void)0;
#endif
}

esp_err_t ae_device_start(void)
{
    if (s_active) {
        return ESP_OK;
    }
    memset(&s_diag, 0, sizeof(s_diag));
    s_diag_publish_count = 0u;
    ae_config_t acfg;
    ae_config_set_defaults(&acfg);
    if (!ae_start(&s_ae, &acfg)) {
        return ESP_FAIL;
    }
    /* Baseline: ignore overwrite events that happened before AE start. */
    s_last_overwrite_seen = sp_mic_pcm_pending_overwrite();
    s_active = true;
    ESP_LOGI(TAG, "Audio Engine device path started (AE only, M5 stereo foundation)");
    return ESP_OK;
}

void ae_device_stop(void)
{
    if (!s_active) {
        return;
    }
    s_active = false;
    memset(&s_ae, 0, sizeof(s_ae));
    memset(&s_diag, 0, sizeof(s_diag));
    s_diag_publish_count = 0u;
    s_last_overwrite_seen = 0u;
    ESP_LOGI(TAG, "Audio Engine device path stopped");
}

bool ae_device_active(void)
{
    return s_active;
}

esp_err_t ae_device_poll(void)
{
    if (!s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sp_mic_pcm_ready()) {
        return ESP_OK;
    }

    /* Coherent PCM + channel snapshots (same window_seq / measured_at).
     * Do not use get_channel() here — that can race s_latest publication. */
    sp_mic_window_job_t wjob;
    if (!sp_mic_take_window_job(&wjob)) {
        s_diag.no_job++;
        return ESP_OK;
    }
    const sp_mic_pcm_job_t *job = &wjob.pcm;
    s_diag.pcm_jobs_taken++;
    s_diag.last_pcm_seq = job->window_seq;
    s_diag.last_pcm_end_us = job->window_end_mono_us;

    if (job->samples == NULL || job->sample_count == 0) {
        s_diag.analyze_fail++;
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t an = dbmap_ear_acoustic_analyze_pcm_job(job);
    if (an != ESP_OK) {
        s_diag.analyze_fail++;
        return an;
    }
    s_diag.analyze_ok++;

    const bool ch0_ok = (wjob.channel_mask & 0x01u) != 0u;
    const bool ch1_ok = (wjob.channel_mask & 0x02u) != 0u;

    ae_align_input_t ain;
    memset(&ain, 0, sizeof(ain));
    ain.pcm_window_seq = job->window_seq;
    ain.pcm_end_mono_us = job->window_end_mono_us;
    ain.pcm_seq_valid = (job->window_seq != 0u);
    fill_snap(&wjob.channels[0], ch0_ok, &ain.mic0);
    fill_snap(&wjob.channels[1], ch1_ok, &ain.mic1);

    ae_align_result_t ares;
    ae_align_channels(&ain, &ares);
    s_diag.last_mismatch_flags = ares.mismatch_flags;
    if (ares.mic0_aligned) {
        s_diag.mic0_aligned++;
    }
    if (ares.mic1_aligned) {
        s_diag.mic1_aligned++;
    }
    if (ares.pair_aligned) {
        s_diag.pair_aligned++;
    } else if (ares.mismatch_flags != 0u
               || (ain.mic0.present && ain.mic1.present)) {
        s_diag.mismatch_reject_pair++;
    }

    {
        const bool expect_stereo = (sp_mic_pcm_retained_channels() >= 2u)
            || (wjob.live_mic_count >= 2u);
        const ae_stereo_eval_t st = ae_stereo_eval(
            &ares,
            expect_stereo,
            job->pcm_channel_mask,
            job->sample_count_ch0,
            job->sample_count_ch1
        );
        s_diag.last_stereo_state = (uint8_t)st.state;
        s_diag.last_pcm_mask = st.pcm_channel_mask;
        s_diag.last_pcm_n0 = st.pcm_n0;
        s_diag.last_pcm_n1 = st.pcm_n1;
        s_diag.last_mic0_rms_dbfs = ain.mic0.present ? ain.mic0.rms_dbfs : 0.0f;
        s_diag.last_mic1_rms_dbfs = ain.mic1.present ? ain.mic1.rms_dbfs : 0.0f;
        switch (st.state) {
        case AE_STEREO_OK:
            s_diag.stereo_ok++;
            break;
        case AE_STEREO_MONO_FALLBACK:
            s_diag.stereo_mono_fallback++;
            break;
        case AE_STEREO_MISALIGNED:
            s_diag.stereo_misaligned++;
            break;
        case AE_STEREO_DEGRADED:
            s_diag.stereo_degraded++;
            break;
        case AE_STEREO_PCM_INCOMPLETE:
            s_diag.stereo_pcm_incomplete++;
            break;
        default:
            break;
        }
    }

    dbmap_l0_window_t l0;
    dbmap_l1_window_t l1;
    dbmap_l2_window_t l2;
    memset(&l0, 0, sizeof(l0));
    memset(&l1, 0, sizeof(l1));
    memset(&l2, 0, sizeof(l2));
    (void)dbmap_ear_mic_get_l0_window(&l0);
#if defined(CONFIG_DBMAP_EAR_OBS_L1) && CONFIG_DBMAP_EAR_OBS_L1
    (void)dbmap_ear_mic_get_l1_window(&l1);
#if defined(CONFIG_DBMAP_EAR_OBS_L2) && CONFIG_DBMAP_EAR_OBS_L2
    (void)dbmap_ear_mic_get_l2_window(&l2);
#endif
#endif

    /* Feature windows must match PCM end time (same analyze job). */
    const dbmap_l0_window_t *l0p =
        (l0.valid && l0.window_end_mono_us == job->window_end_mono_us) ? &l0 : NULL;
    const dbmap_l1_window_t *l1p =
        (l1.valid && l1.window_end_mono_us == job->window_end_mono_us) ? &l1 : NULL;
    const dbmap_l2_window_t *l2p =
        (l2.eligible && l2.window_end_mono_us == job->window_end_mono_us) ? &l2 : NULL;

    if (l0p == NULL) {
        /* Should not happen after successful analyze; skip signature. */
        s_diag.analyze_fail++;
        return ESP_OK;
    }

    const uint32_t last = ae_last_window_seq(&s_ae);
    const uint32_t ow_now = sp_mic_pcm_pending_overwrite();
    const ae_window_gap_t gap = ae_window_gap_eval(
        last,
        job->window_seq,
        ow_now,
        &s_last_overwrite_seen
    );
    if (gap.overwrite_delta > 0u) {
        s_diag.pcm_overwrite_events += gap.overwrite_delta;
    }
    if (gap.seq_gap) {
        s_diag.seq_gap_events++;
    }

    ae_signature_t sig;
    const ae_pair_metrics_t *pair =
        ares.pair_aligned ? &ares.pair : NULL;
    if (!ae_signature_build(
            job->window_seq != 0u ? job->window_seq : 1u,
            l0p,
            l1p,
            l2p,
            pair,
            gap.gap_before,
            &sig
        )) {
        return ESP_OK;
    }

    /* If channels mismatched, never claim pair — already pair=NULL.
     * Mono signature still uses PCM-tied L0/L1/L2. */
    (void)ae_publish_signature(&s_ae, &sig);
    s_diag.sig_published++;
    s_diag.last_quality = sig.quality;
    if (gap.gap_before) {
        s_diag.gap_sigs_published++;
    }
    s_diag_publish_count++;
    maybe_diag_log();
    return ESP_OK;
}

void ae_device_get_diag(ae_device_diag_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_diag;
}

ae_engine_t *ae_device_engine(void)
{
    return s_active ? &s_ae : NULL;
}

#else /* !CONFIG_DBMAP_EAR_AUDIO_ENGINE */

esp_err_t ae_device_start(void)
{
    return ESP_OK;
}

void ae_device_stop(void)
{
}

bool ae_device_active(void)
{
    return false;
}

esp_err_t ae_device_poll(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void ae_device_get_diag(ae_device_diag_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

ae_engine_t *ae_device_engine(void)
{
    return NULL;
}

#endif
