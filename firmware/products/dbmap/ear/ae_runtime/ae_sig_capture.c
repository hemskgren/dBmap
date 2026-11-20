#include "ae_sig_capture.h"

#include <stdio.h>
#include <string.h>

#if defined(CONFIG_DBMAP_EAR_AE_SIG_CAPTURE) && CONFIG_DBMAP_EAR_AE_SIG_CAPTURE

#include "as_profile.h"

static ae_sig_capture_rec_t s_buf[AE_SIG_CAPTURE_CAPACITY];
static ae_sig_capture_diag_t s_diag;
/** Diagnostic AS-001 relative-bg state (mirrors Signature Engine eligibility). */
static as_profile_bg_state_t s_as001_bg;

bool ae_sig_capture_compiled(void)
{
    return true;
}

uint32_t ae_sig_capture_capacity(void)
{
    return AE_SIG_CAPTURE_CAPACITY;
}

void ae_sig_capture_reset(uint32_t session_id)
{
    memset(s_buf, 0, sizeof(s_buf));
    memset(&s_diag, 0, sizeof(s_diag));
    as_profile_bg_init(&s_as001_bg);
    s_diag.session_id = session_id;
    s_diag.settle_remaining = AE_SIG_CAPTURE_SETTLE_N;
    s_diag.state = (AE_SIG_CAPTURE_SETTLE_N > 0u) ? AE_SIG_CAPTURE_SETTLING
                                                 : AE_SIG_CAPTURE_ACTIVE;
}

void ae_sig_capture_get_diag(ae_sig_capture_diag_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_diag;
}

int ae_sig_capture_format_csv(
    const ae_sig_capture_rec_t *rec,
    uint32_t session_id,
    char *out,
    size_t out_cap
)
{
    if (rec == NULL || out == NULL || out_cap == 0u) {
        return -1;
    }
    return snprintf(
        out,
        out_cap,
        "%lu,%lu,%lld,%lu,16000,"
        "%u,%u,0x%02x,%u,"
        "%.4f,%.4f,%.4f,"
        "%.4f,%.4f,%.4f,%.4f,"
        "%.4f,%.4f,%.4f,%.4f,"
        "%.4f,%u,%u,%.4f,%.4f,%.4f,"
        "%.6f,%u,%u,%u,%u,"
        "%.4f",
        (unsigned long)session_id,
        (unsigned long)rec->window_seq,
        (long long)rec->window_end_mono_us,
        (unsigned long)rec->window_ms,
        (unsigned)rec->channel,
        (unsigned)rec->quality,
        (unsigned)rec->flags,
        (unsigned)rec->clipped,
        (double)rec->rms_dbfs,
        (double)rec->peak_dbfs,
        (double)rec->crest_db,
        (double)rec->band_dbfs[0],
        (double)rec->band_dbfs[1],
        (double)rec->band_dbfs[2],
        (double)rec->band_dbfs[3],
        (double)rec->band_rel[0],
        (double)rec->band_rel[1],
        (double)rec->band_rel[2],
        (double)rec->band_rel[3],
        (double)rec->hl_db,
        (unsigned)rec->hl_valid,
        (unsigned)rec->spectral_valid,
        (double)rec->centroid_hz,
        (double)rec->bandwidth_hz,
        (double)rec->dominant_hz,
        (double)rec->flatness,
        (unsigned)rec->as001_matched,
        (unsigned)rec->as001_rej_elig,
        (unsigned)rec->as001_skip_spec,
        (unsigned)rec->as001_profile_id,
        (double)rec->as001_dist
    );
}

static void fill_rec(const ae_signature_t *sig, ae_sig_capture_rec_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    rec->window_seq = sig->window_seq;
    rec->window_end_mono_us = sig->window_end_mono_us;
    rec->window_ms = sig->window_ms != 0u ? sig->window_ms : 1000u;
    rec->channel = 0u; /* AE signature uses L0 / mic0 path */
    rec->quality = sig->quality;
    rec->flags = sig->flags;
    rec->clipped = (sig->flags & AE_FLAG_CLIPPED) != 0u ? 1u : 0u;
    rec->rms_dbfs = sig->rms_dbfs;
    rec->peak_dbfs = sig->peak_dbfs;
    rec->crest_db = sig->crest_db;
    for (uint32_t i = 0; i < AE_BAND_COUNT; i++) {
        rec->band_dbfs[i] = sig->band_dbfs[i];
        rec->band_rel[i] = sig->band_dbfs[i] - sig->rms_dbfs;
    }
    rec->hl_db = sig->hl_db;
    rec->hl_valid = sig->hl_valid ? 1u : 0u;
    rec->spectral_valid = sig->spectral_valid ? 1u : 0u;
    rec->centroid_hz = sig->centroid_hz;
    rec->bandwidth_hz = sig->bandwidth_hz;
    rec->dominant_hz = sig->dominant_hz;
    rec->flatness = sig->flatness;

    /*
     * Pure diagnostic: AS-001 match with relative-bg (no CE/Obs/MQTT side
     * effects). Uses a capture-local EMA so CSV columns track the same
     * eligibility principle as the Signature Engine.
     */
    as_profile_match_result_t mr;
    const as_profile_t *p = as_profile_as001();
    const bool matched = as_profile_match_with_bg(p, sig, &s_as001_bg, &mr);
    rec->as001_matched = matched ? 1u : 0u;
    rec->as001_rej_elig = mr.rejected_eligibility;
    rec->as001_skip_spec = mr.skipped_spectral;
    rec->as001_profile_id = AS_PROFILE_ID_AS001;
    rec->as001_dist = mr.distance;
}

void ae_sig_capture_on_signature(const ae_signature_t *sig)
{
    if (sig == NULL) {
        return;
    }
    if (s_diag.state == AE_SIG_CAPTURE_DISABLED) {
        return;
    }
    /* After complete: count further windows as overflow (never silent). */
    if (s_diag.state == AE_SIG_CAPTURE_COMPLETE) {
        s_diag.overflow++;
        return;
    }
    if (s_diag.state == AE_SIG_CAPTURE_SETTLING) {
        if (s_diag.settle_remaining > 0u) {
            s_diag.settle_remaining--;
        }
        if (s_diag.settle_remaining == 0u) {
            s_diag.state = AE_SIG_CAPTURE_ACTIVE;
        }
        /*
         * Settle windows are not stored, but must still advance the
         * capture-local relative-bg EMA. Otherwise the first stored rows
         * seed baseline from mid-event levels and diverge from the
         * Signature Engine (which processes every published window).
         */
        {
            as_profile_match_result_t mr;
            (void)as_profile_match_with_bg(
                as_profile_as001(), sig, &s_as001_bg, &mr
            );
        }
        return;
    }
    if (s_diag.stored >= AE_SIG_CAPTURE_CAPACITY) {
        s_diag.overflow++;
        s_diag.state = AE_SIG_CAPTURE_COMPLETE;
        return;
    }
    fill_rec(sig, &s_buf[s_diag.stored]);
    s_diag.stored++;
    if (s_diag.stored >= AE_SIG_CAPTURE_CAPACITY) {
        s_diag.state = AE_SIG_CAPTURE_COMPLETE;
    }
}

bool ae_sig_capture_drain_one(char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0u) {
        return false;
    }
    if (s_diag.drained >= s_diag.stored) {
        return false;
    }
    const int n = ae_sig_capture_format_csv(
        &s_buf[s_diag.drained], s_diag.session_id, out, out_cap
    );
    if (n < 0 || (size_t)n >= out_cap) {
        out[0] = '\0';
        return false;
    }
    s_diag.drained++;
    return true;
}

bool ae_sig_capture_take_start_marker(void)
{
    if (s_diag.state != AE_SIG_CAPTURE_ACTIVE && s_diag.state != AE_SIG_CAPTURE_COMPLETE) {
        return false;
    }
    if (s_diag.started_logged) {
        return false;
    }
    s_diag.started_logged = true;
    return true;
}

bool ae_sig_capture_take_end_marker(void)
{
    if (s_diag.state != AE_SIG_CAPTURE_COMPLETE) {
        return false;
    }
    if (s_diag.drained < s_diag.stored) {
        return false;
    }
    if (s_diag.ended_logged) {
        return false;
    }
    s_diag.ended_logged = true;
    return true;
}

#else /* !CONFIG_DBMAP_EAR_AE_SIG_CAPTURE */

bool ae_sig_capture_compiled(void)
{
    return false;
}

uint32_t ae_sig_capture_capacity(void)
{
    return 0u;
}

void ae_sig_capture_reset(uint32_t session_id)
{
    (void)session_id;
}

void ae_sig_capture_on_signature(const ae_signature_t *sig)
{
    (void)sig;
}

bool ae_sig_capture_drain_one(char *out, size_t out_cap)
{
    (void)out;
    (void)out_cap;
    return false;
}

void ae_sig_capture_get_diag(ae_sig_capture_diag_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->state = AE_SIG_CAPTURE_DISABLED;
    }
}

bool ae_sig_capture_take_start_marker(void)
{
    return false;
}

bool ae_sig_capture_take_end_marker(void)
{
    return false;
}

int ae_sig_capture_format_csv(
    const ae_sig_capture_rec_t *rec,
    uint32_t session_id,
    char *out,
    size_t out_cap
)
{
    (void)rec;
    (void)session_id;
    if (out != NULL && out_cap > 0u) {
        out[0] = '\0';
    }
    return -1;
}

#endif
