#include "ae_history.h"

#include <string.h>

void ae_history_init(ae_history_t *hist)
{
    if (hist == NULL) {
        return;
    }
    memset(hist, 0, sizeof(*hist));
}

void ae_history_push(ae_history_t *hist, const ae_signature_t *sig)
{
    if (hist == NULL || sig == NULL) {
        return;
    }
    hist->entries[hist->next] = *sig;
    hist->next = (hist->next + 1u) % AE_HISTORY_CAPACITY;
    if (hist->count < AE_HISTORY_CAPACITY) {
        hist->count++;
    }
    hist->pushed++;
}

bool ae_history_latest(const ae_history_t *hist, ae_signature_t *out)
{
    if (hist == NULL || out == NULL || hist->count == 0u) {
        return false;
    }
    uint32_t idx = (hist->next + AE_HISTORY_CAPACITY - 1u) % AE_HISTORY_CAPACITY;
    *out = hist->entries[idx];
    return true;
}

uint32_t ae_history_copy_newest(
    const ae_history_t *hist,
    ae_signature_t *out,
    uint32_t max_n
)
{
    if (hist == NULL || out == NULL || max_n == 0u || hist->count == 0u) {
        return 0u;
    }
    uint32_t n = hist->count < max_n ? hist->count : max_n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx =
            (hist->next + AE_HISTORY_CAPACITY - 1u - i) % AE_HISTORY_CAPACITY;
        out[i] = hist->entries[idx];
    }
    return n;
}

uint32_t ae_history_copy_since_mono(
    const ae_history_t *hist,
    int64_t min_end_mono_us,
    ae_signature_t *out,
    uint32_t max_n
)
{
    if (hist == NULL || out == NULL || max_n == 0u || hist->count == 0u) {
        return 0u;
    }
    uint32_t copied = 0u;
    for (uint32_t i = 0; i < hist->count && copied < max_n; i++) {
        uint32_t idx =
            (hist->next + AE_HISTORY_CAPACITY - 1u - i) % AE_HISTORY_CAPACITY;
        const ae_signature_t *sig = &hist->entries[idx];
        if (sig->window_end_mono_us >= min_end_mono_us) {
            out[copied++] = *sig;
        }
    }
    return copied;
}

uint32_t ae_history_expire_before(ae_history_t *hist, int64_t min_end_mono_us)
{
    if (hist == NULL || hist->count == 0u) {
        return 0u;
    }

    ae_signature_t keep[AE_HISTORY_CAPACITY];
    uint32_t keep_n = 0u;
    /* Oldest → newest so push rebuild preserves chronology. */
    for (uint32_t i = 0; i < hist->count; i++) {
        uint32_t idx =
            (hist->next + AE_HISTORY_CAPACITY - hist->count + i) % AE_HISTORY_CAPACITY;
        if (hist->entries[idx].window_end_mono_us >= min_end_mono_us) {
            keep[keep_n++] = hist->entries[idx];
        }
    }
    const uint32_t removed = hist->count - keep_n;
    const uint32_t pushed = hist->pushed;
    ae_history_init(hist);
    hist->pushed = pushed;
    for (uint32_t i = 0; i < keep_n; i++) {
        ae_history_push(hist, &keep[i]);
    }
    /* Restore pushed to pre-rebuild total (push increments). */
    hist->pushed = pushed;
    return removed;
}
