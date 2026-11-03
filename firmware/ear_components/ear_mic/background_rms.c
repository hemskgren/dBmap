#include "dbmap_background_rms.h"

#include <math.h>
#include <string.h>

void dbmap_bg_rms_hist_init(
    dbmap_bg_rms_hist_t *hist,
    dbmap_bg_rms_sample_t *entries,
    uint32_t capacity,
    uint32_t window_ms
)
{
    if (hist == NULL) {
        return;
    }
    memset(hist, 0, sizeof(*hist));
    hist->entries = entries;
    hist->capacity = capacity;
    hist->window_ms = window_ms;
    if (entries != NULL && capacity > 0) {
        memset(entries, 0, sizeof(entries[0]) * capacity);
    }
}

bool dbmap_bg_rms_should_include(const dbmap_bg_rms_candidate_t *c, uint32_t boot_settle_ms)
{
    if (c == NULL || !c->levels_valid) {
        return false;
    }
    if (!isfinite((double)c->rms_dbfs) || c->rms_dbfs > 0.0f) {
        return false;
    }
    if (!isfinite((double)c->peak_dbfs)) {
        return false;
    }
    if (c->transport_status == NULL || c->microphone_status == NULL) {
        return false;
    }
    if (strcmp(c->transport_status, "ok") != 0 && strcmp(c->transport_status, "degraded") != 0) {
        return false;
    }
    if (strcmp(c->microphone_status, "unavailable") == 0 ||
        strcmp(c->microphone_status, "disconnected_suspected") == 0 ||
        strcmp(c->microphone_status, "noise_suspected") == 0) {
        return false;
    }
    /* Clipped: existing mic clipped_samples and/or L0-equivalent peak rule. */
    if (c->clipped_samples > 0 || c->peak_dbfs >= DBMAP_BG_RMS_CLIP_PEAK_DBFS) {
        return false;
    }
    /* Abnormal first I2S window(s): existing window_seq + settle time. */
    if (c->window_seq <= 1u) {
        return false;
    }
    if (boot_settle_ms > 0 && c->ms_since_i2s_enable < boot_settle_ms) {
        return false;
    }
    return true;
}

static void drop_expired(dbmap_bg_rms_hist_t *hist, int64_t now_mono_us)
{
    if (hist == NULL || hist->entries == NULL || hist->count == 0 || hist->window_ms == 0) {
        return;
    }
    const int64_t cutoff = now_mono_us - ((int64_t)hist->window_ms * 1000LL);
    while (hist->count > 0) {
        const uint32_t oldest =
            (hist->head + hist->capacity - hist->count) % hist->capacity;
        if (hist->entries[oldest].t_end_mono_us >= cutoff) {
            break;
        }
        hist->count--;
    }
}

void dbmap_bg_rms_push(dbmap_bg_rms_hist_t *hist, float rms_dbfs, int64_t t_end_mono_us)
{
    if (hist == NULL || hist->entries == NULL || hist->capacity == 0) {
        return;
    }
    if (!isfinite((double)rms_dbfs) || rms_dbfs > 0.0f || t_end_mono_us < 0) {
        return;
    }

    if (hist->collection_start_mono_us == 0) {
        hist->collection_start_mono_us = t_end_mono_us;
    }

    drop_expired(hist, t_end_mono_us);

    if (hist->count == hist->capacity) {
        /* Overwrite oldest when full (should be rare if capacity matches window). */
        hist->count--;
    }
    hist->entries[hist->head].rms_dbfs = rms_dbfs;
    hist->entries[hist->head].t_end_mono_us = t_end_mono_us;
    hist->head = (hist->head + 1u) % hist->capacity;
    hist->count++;
}

bool dbmap_bg_rms_linear_mean_dbfs(const float *rms_dbfs, uint32_t count, float *out_mean_dbfs)
{
    if (rms_dbfs == NULL || out_mean_dbfs == NULL || count == 0) {
        return false;
    }
    double sum_e = 0.0;
    for (uint32_t i = 0; i < count; ++i) {
        sum_e += pow(10.0, (double)rms_dbfs[i] / 10.0);
    }
    const double mean_e = sum_e / (double)count;
    if (!(mean_e > 0.0)) {
        return false;
    }
    *out_mean_dbfs = (float)(10.0 * log10(mean_e));
    return true;
}

void dbmap_bg_rms_profile(
    const dbmap_bg_rms_hist_t *hist,
    int64_t now_mono_us,
    dbmap_bg_rms_profile_t *out
)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->present = false;
    out->status = "partial";

    if (hist == NULL || hist->entries == NULL || hist->window_ms == 0 || hist->count == 0) {
        return;
    }

    const int64_t cutoff = now_mono_us - ((int64_t)hist->window_ms * 1000LL);
    float min_v = 0.0f;
    float max_v = 0.0f;
    bool have = false;
    double sum_e = 0.0;
    uint32_t n = 0;

    for (uint32_t i = 0; i < hist->count; ++i) {
        const uint32_t idx =
            (hist->head + hist->capacity - hist->count + i) % hist->capacity;
        const dbmap_bg_rms_sample_t *s = &hist->entries[idx];
        if (s->t_end_mono_us < cutoff) {
            continue; /* time filter only — do not mutate ring */
        }
        if (!have) {
            min_v = s->rms_dbfs;
            max_v = s->rms_dbfs;
            have = true;
        } else {
            if (s->rms_dbfs < min_v) {
                min_v = s->rms_dbfs;
            }
            if (s->rms_dbfs > max_v) {
                max_v = s->rms_dbfs;
            }
        }
        sum_e += pow(10.0, (double)s->rms_dbfs / 10.0);
        n++;
    }

    if (n == 0 || !have) {
        return;
    }

    const double mean_e = sum_e / (double)n;
    if (!(mean_e > 0.0)) {
        return;
    }

    out->present = true;
    out->window_ms = hist->window_ms;
    out->sample_count = n;
    out->rms_min_dbfs = min_v;
    out->rms_max_dbfs = max_v;
    out->rms_mean_dbfs = (float)(10.0 * log10(mean_e));

    int64_t elapsed_us = 0;
    if (hist->collection_start_mono_us > 0 && now_mono_us >= hist->collection_start_mono_us) {
        elapsed_us = now_mono_us - hist->collection_start_mono_us;
    }
    const int64_t need_us = (int64_t)hist->window_ms * 1000LL;
    out->status = (elapsed_us >= need_us) ? "full" : "partial";
}
