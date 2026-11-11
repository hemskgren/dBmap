#include "proto_pre_event_baseline.h"

#include <math.h>
#include <string.h>

void proto_pre_event_baseline_init(proto_pre_event_baseline_t *b, uint32_t stale_max_ms)
{
    if (b == NULL) {
        return;
    }
    memset(b, 0, sizeof(*b));
    b->stale_max_ms = stale_max_ms;
}

static void drop_stale(proto_pre_event_baseline_t *b, int64_t now_us)
{
    if (b == NULL || b->stale_max_ms == 0 || b->count == 0) {
        return;
    }
    const int64_t cutoff = now_us - ((int64_t)b->stale_max_ms * 1000LL);
    while (b->count > 0) {
        const uint32_t oldest =
            (b->head + PROTO_PRE_EVENT_BASELINE_CAP - b->count) % PROTO_PRE_EVENT_BASELINE_CAP;
        if (b->entries[oldest].t_end_mono_us >= cutoff) {
            break;
        }
        b->entries[oldest].valid = false;
        b->count--;
    }
}

void proto_pre_event_baseline_push(
    proto_pre_event_baseline_t *b,
    float rms_dbfs,
    int64_t t_end_mono_us
)
{
    if (b == NULL || b->frozen) {
        return;
    }
    if (!isfinite((double)rms_dbfs) || rms_dbfs > 0.0f || t_end_mono_us < 0) {
        return;
    }

    drop_stale(b, t_end_mono_us);

    if (b->count == PROTO_PRE_EVENT_BASELINE_CAP) {
        b->count--;
    }
    b->entries[b->head].rms_dbfs = rms_dbfs;
    b->entries[b->head].t_end_mono_us = t_end_mono_us;
    b->entries[b->head].valid = true;
    b->head = (b->head + 1u) % PROTO_PRE_EVENT_BASELINE_CAP;
    b->count++;
}

float proto_pre_event_baseline_median(const float *values, uint32_t n)
{
    if (values == NULL || n == 0) {
        return 0.0f;
    }
    float tmp[PROTO_PRE_EVENT_BASELINE_CAP];
    if (n > PROTO_PRE_EVENT_BASELINE_CAP) {
        n = PROTO_PRE_EVENT_BASELINE_CAP;
    }
    memcpy(tmp, values, sizeof(float) * n);
    /* Insertion sort ascending. */
    for (uint32_t i = 1; i < n; ++i) {
        float key = tmp[i];
        int j = (int)i - 1;
        while (j >= 0 && tmp[j] > key) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
    }
    if ((n % 2u) == 1u) {
        return tmp[n / 2u];
    }
    return 0.5f * (tmp[n / 2u - 1u] + tmp[n / 2u]);
}

bool proto_pre_event_baseline_freeze(
    proto_pre_event_baseline_t *b,
    int64_t episode_start_us
)
{
    if (b == NULL) {
        return false;
    }
    drop_stale(b, episode_start_us);

    float vals[PROTO_PRE_EVENT_BASELINE_CAP];
    uint32_t n = 0;
    for (uint32_t i = 0; i < b->count; ++i) {
        const uint32_t idx =
            (b->head + PROTO_PRE_EVENT_BASELINE_CAP - b->count + i) % PROTO_PRE_EVENT_BASELINE_CAP;
        const proto_pre_event_sample_t *s = &b->entries[idx];
        if (!s->valid) {
            continue;
        }
        /* Pre-event only: window ended at or before episode start. */
        if (s->t_end_mono_us > episode_start_us) {
            continue;
        }
        vals[n++] = s->rms_dbfs;
    }

    b->frozen = true;
    b->frozen_sample_count = n;
    if (n >= PROTO_PRE_EVENT_BASELINE_MIN_SAMPLES) {
        b->reliable = true;
        b->frozen_baseline_dbfs = proto_pre_event_baseline_median(vals, n);
        return true;
    }
    b->reliable = false;
    b->frozen_baseline_dbfs = 0.0f;
    return false;
}

void proto_pre_event_baseline_clear(proto_pre_event_baseline_t *b)
{
    if (b == NULL) {
        return;
    }
    const uint32_t stale = b->stale_max_ms;
    memset(b, 0, sizeof(*b));
    b->stale_max_ms = stale;
}
