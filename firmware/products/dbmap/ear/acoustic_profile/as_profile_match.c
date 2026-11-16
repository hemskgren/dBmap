#include "as_profile.h"

#include <math.h>
#include <string.h>

/* Weights aligned with CE Stage 1 identity scales (profile vs template). */
#define AS_W_RMS 0.20f
#define AS_W_BAND 0.30f
#define AS_W_DOM_HZ 0.02f
#define AS_W_FLAT 3.0f
#define AS_W_CENT 0.01f
#define AS_W_PAIR 0.25f

static bool spectral_gate_ok(const as_profile_t *p, const ae_signature_t *sig)
{
    if (p->dominant_hz_max >= p->dominant_hz_min) {
        if (sig->dominant_hz < p->dominant_hz_min
            || sig->dominant_hz > p->dominant_hz_max) {
            return false;
        }
    }
    if (p->flatness_max >= p->flatness_min) {
        if (sig->flatness < p->flatness_min || sig->flatness > p->flatness_max) {
            return false;
        }
    }
    return true;
}

bool as_profile_match(
    const as_profile_t *profile,
    const ae_signature_t *sig,
    as_profile_match_result_t *out
)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (profile == NULL || sig == NULL) {
        return false;
    }
    if (sig->schema_version != 0u
        && sig->schema_version != profile->ae_schema_version) {
        if (out != NULL) {
            out->rejected_eligibility = 1u;
        }
        return false;
    }
    if (sig->quality != AE_QUALITY_OK && sig->quality != AE_QUALITY_DEGRADED) {
        if (out != NULL) {
            out->rejected_eligibility = 1u;
        }
        return false;
    }

    as_profile_match_result_t local;
    memset(&local, 0, sizeof(local));
    local.profile_id = profile->id;
    local.acoustic_class = profile->acoustic_class;

    if (profile->uses_rms && sig->rms_dbfs < profile->rms_min_dbfs) {
        local.rejected_eligibility = 1u;
        if (out != NULL) {
            *out = local;
        }
        return false;
    }

    if (profile->uses_spectral) {
        if (!sig->spectral_valid) {
            local.skipped_spectral = 1u;
            local.rejected_eligibility = 1u;
            if (out != NULL) {
                *out = local;
            }
            return false;
        }
        if (!spectral_gate_ok(profile, sig)) {
            local.rejected_eligibility = 1u;
            if (out != NULL) {
                *out = local;
            }
            return false;
        }
    }

    float dist = 0.0f;

    if (profile->uses_rms) {
        dist += AS_W_RMS * fabsf(profile->rms_center_dbfs - sig->rms_dbfs);
    }

    if (profile->uses_bands) {
        for (uint32_t i = 0; i < AE_BAND_COUNT; i++) {
            const float shape_s = sig->band_dbfs[i] - sig->rms_dbfs;
            dist += AS_W_BAND * fabsf(profile->band_rel_center[i] - shape_s);
        }
    }

    if (profile->uses_spectral && sig->spectral_valid) {
        dist += AS_W_DOM_HZ * fabsf(profile->dominant_hz_center - sig->dominant_hz);
        dist += AS_W_FLAT * fabsf(profile->flatness_center - sig->flatness);
        dist += AS_W_CENT * fabsf(profile->centroid_hz_center - sig->centroid_hz);
    } else if (profile->uses_spectral) {
        local.skipped_spectral = 1u;
    }

    /*
     * Pair: only when the profile declares pair features AND the signature
     * has valid pair metrics. AS-001 has uses_pair=false — mono derivation
     * must not invent ΔRMS comparisons. If a future profile enables pair but
     * the live signature lacks pair_valid, skip the term (do not fail match
     * solely for missing pair).
     */
    if (profile->uses_pair) {
        if (sig->pair_valid) {
            dist += AS_W_PAIR
                * fabsf(profile->delta_rms_db_center - sig->delta_rms_db);
        } else {
            local.skipped_pair = 1u;
        }
    } else {
        local.skipped_pair = 1u; /* profile does not use pair */
    }

    local.distance = dist;
    local.matched = (dist <= profile->match_distance_max);

    if (out != NULL) {
        *out = local;
    }
    return local.matched;
}

bool as_profile_match_best(
    const ae_signature_t *sig,
    as_profile_match_result_t *out
)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (sig == NULL) {
        return false;
    }

    as_profile_match_result_t best;
    memset(&best, 0, sizeof(best));
    bool have = false;

    const size_t n = as_profile_count();
    for (size_t i = 0; i < n; i++) {
        const as_profile_t *p = as_profile_get_by_index(i);
        as_profile_match_result_t r;
        if (!as_profile_match(p, sig, &r) || !r.matched) {
            continue;
        }
        if (!have || r.distance < best.distance) {
            best = r;
            have = true;
        }
    }

    if (out != NULL) {
        *out = best;
    }
    return have;
}
