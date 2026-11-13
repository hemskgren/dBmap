#include "ae_signature.h"

#include <math.h>
#include <string.h>

bool ae_signature_build(
    uint32_t window_seq,
    const dbmap_l0_window_t *l0,
    const dbmap_l1_window_t *l1,
    const dbmap_l2_window_t *l2,
    const ae_pair_metrics_t *pair,
    bool gap_before,
    ae_signature_t *out
)
{
    if (out == NULL || l0 == NULL || window_seq == 0u) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->schema_version = AE_SIGNATURE_SCHEMA_VERSION;
    out->window_seq = window_seq;
    out->window_end_mono_us = l0->window_end_mono_us;
    out->window_ms = l0->window_ms;
    out->mic_mask = 0x01u;

    /* Acquisition-tied end time is mandatory for a usable CE clock. */
    if (!l0->valid || l0->window_end_mono_us <= 0) {
        out->quality = AE_QUALITY_INVALID;
        return true;
    }

    out->rms_dbfs = l0->rms_dbfs;
    out->peak_dbfs = l0->peak_dbfs;
    out->crest_db = l0->crest_db;
    for (uint32_t i = 0; i < AE_BAND_COUNT; i++) {
        out->band_dbfs[i] = l0->band_dbfs[i];
    }
    out->hl_valid = l0->hl_valid;
    out->hl_db = l0->hl_db;
    if (l0->clipped) {
        out->flags |= AE_FLAG_CLIPPED;
    }

    if (l1 != NULL && l1->valid && !l1->clipped) {
        out->centroid_hz = l1->centroid_hz;
        out->bandwidth_hz = l1->bandwidth_hz;
        out->dominant_hz = l1->dominant_hz;
        out->flatness = l1->flatness;
        out->spectral_valid = true;
    } else {
        out->flags |= AE_FLAG_L1_MISSING;
    }

    if (l2 != NULL && l2->eligible) {
        out->spectral_flux = l2->spectral_flux;
        out->envelope_cv = l2->envelope_cv;
        out->dynamics_valid = true;
        out->dominant_delta_valid = l2->has_dominant_delta;
        out->dominant_delta_hz = l2->dominant_delta_hz;
    } else {
        out->flags |= AE_FLAG_L2_MISSING;
    }

    if (pair != NULL && pair->valid) {
        out->pair_valid = true;
        out->delta_rms_db = pair->mic0_rms_dbfs - pair->mic1_rms_dbfs;
        out->delta_peak_db = pair->mic0_peak_dbfs - pair->mic1_peak_dbfs;
        out->mic_mask = pair->mic_mask != 0u ? pair->mic_mask : 0x03u;
        if (pair->stereo_degraded) {
            out->flags |= AE_FLAG_STEREO_DEGRADED;
        }
    } else {
        out->flags |= AE_FLAG_PAIR_MISSING;
    }

    if (gap_before) {
        out->quality = AE_QUALITY_GAP;
    } else if ((out->flags & AE_FLAG_CLIPPED) != 0u
               || (out->flags & AE_FLAG_STEREO_DEGRADED) != 0u
               || (out->flags & AE_FLAG_L1_MISSING) != 0u) {
        out->quality = AE_QUALITY_DEGRADED;
    } else {
        out->quality = AE_QUALITY_OK;
    }

    return true;
}

float ae_signature_distance(const ae_signature_t *a, const ae_signature_t *b)
{
    if (a == NULL || b == NULL) {
        return 1.0e6f;
    }

    float dist = fabsf(a->rms_dbfs - b->rms_dbfs);

    for (uint32_t i = 0; i < AE_BAND_COUNT; i++) {
        dist += 0.25f * fabsf(a->band_dbfs[i] - b->band_dbfs[i]);
    }

    if (a->spectral_valid && b->spectral_valid) {
        dist += 0.01f * fabsf(a->dominant_hz - b->dominant_hz);
        dist += 2.0f * fabsf(a->flatness - b->flatness);
    }

    if (a->pair_valid && b->pair_valid) {
        dist += 0.15f * fabsf(a->delta_rms_db - b->delta_rms_db);
    }

    return dist;
}

bool ae_signature_usable_for_tracking(const ae_signature_t *sig)
{
    if (sig == NULL) {
        return false;
    }
    if (sig->quality == AE_QUALITY_INVALID) {
        return false;
    }
    /* Gap signatures update coasting; they do not seed new candidates. */
    return true;
}
