#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Observation sequence-number allocator (host-testable).
 *
 * Strategy: reserve contiguous blocks in durable storage; hand out numbers from
 * RAM within the current block. Avoids one NVS write per Observation.
 *
 * Durable value is an exclusive limit (first number NOT reserved). After reboot,
 * next starts at that limit so any unused RAM remainder of the previous block
 * is skipped (gaps OK). Numbers are never reused.
 *
 * Consumption point: a sequence number is consumed when allocate() returns it.
 * That number already lies inside a block whose limit was persisted. A later
 * MQTT publish failure does not un-consume it.
 *
 * Phase 1 legacy NVS key `seq` stored the *next* sequence to assign (persisted
 * before MQTT publish). Migration must not reuse any previously published
 * number; when uncertain, skip forward by a full block.
 */

typedef struct {
    uint32_t next;       /* next number to hand out */
    uint32_t limit;      /* exclusive end of currently reserved block */
    uint32_t block_size; /* numbers reserved per durable write */
} sp_obs_seq_t;

/**
 * Initialize from durable exclusive limit (0 if none).
 * Sets next = limit so unused prior-block remainder is skipped after reboot.
 * Caller must reserve() + persist before the first successful allocate().
 */
void sp_obs_seq_init(sp_obs_seq_t *seq, uint32_t durable_limit, uint32_t block_size);

/**
 * Convert Phase 1 legacy "next sequence" into a durable exclusive limit.
 * Adds one block_size skip-forward so an interrupted publish/crash window
 * cannot reuse a number that may already have been sent.
 * Saturates at UINT32_MAX.
 */
uint32_t sp_obs_seq_migrate_legacy_next(uint32_t legacy_next, uint32_t block_size);

/** True when the next allocate requires extending the reserved block. */
bool sp_obs_seq_needs_reserve(const sp_obs_seq_t *seq);

/**
 * Extend the reserved block by block_size from current next.
 * Returns the new exclusive limit that must be persisted before any allocate
 * that uses numbers from the new block. Saturates at UINT32_MAX (no wrap).
 */
uint32_t sp_obs_seq_reserve(sp_obs_seq_t *seq);

/**
 * Hand out the next sequence number into *out.
 * Returns false if the block is exhausted or exhausted at UINT32_MAX
 * (caller must reserve + persist first, or stop on overflow).
 * On true, *out is consumed permanently (even if the caller never publishes).
 */
bool sp_obs_seq_allocate(sp_obs_seq_t *seq, uint32_t *out);

#ifdef __cplusplus
}
#endif
