#include "dbmap_observation_seq.h"

#include <stddef.h>

void dbmap_obs_seq_init(dbmap_obs_seq_t *seq, uint32_t durable_limit, uint32_t block_size)
{
    if (seq == NULL) {
        return;
    }
    if (block_size == 0) {
        block_size = 1;
    }
    seq->next = durable_limit;
    seq->limit = durable_limit;
    seq->block_size = block_size;
}

uint32_t dbmap_obs_seq_migrate_legacy_next(uint32_t legacy_next, uint32_t block_size)
{
    if (block_size == 0) {
        block_size = 1;
    }
    /*
     * Phase 1 persisted `seq` = next to assign, written before MQTT publish.
     * Numbers < legacy_next are definitely consumed. Skip forward one block so
     * a crash after publish but before an alternate persist path (or any
     * ambiguity) cannot reuse a published sequence_number.
     */
    if (legacy_next > UINT32_MAX - block_size) {
        return UINT32_MAX;
    }
    return legacy_next + block_size;
}

bool dbmap_obs_seq_needs_reserve(const dbmap_obs_seq_t *seq)
{
    if (seq == NULL) {
        return true;
    }
    return seq->next >= seq->limit;
}

uint32_t dbmap_obs_seq_reserve(dbmap_obs_seq_t *seq)
{
    if (seq == NULL) {
        return 0;
    }
    if (seq->block_size == 0) {
        seq->block_size = 1;
    }
    /* Saturate rather than wrap. */
    uint32_t add = seq->block_size;
    if (seq->next > UINT32_MAX - add) {
        seq->limit = UINT32_MAX;
        return seq->limit;
    }
    seq->limit = seq->next + add;
    return seq->limit;
}

bool dbmap_obs_seq_allocate(dbmap_obs_seq_t *seq, uint32_t *out)
{
    if (seq == NULL || out == NULL) {
        return false;
    }
    if (seq->next >= seq->limit) {
        return false;
    }
    /* next == UINT32_MAX cannot be handed out without wrap on increment. */
    if (seq->next == UINT32_MAX) {
        return false;
    }
    *out = seq->next;
    seq->next += 1;
    return true;
}
