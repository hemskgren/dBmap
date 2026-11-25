#pragma once

#include <stdatomic.h>
#include <stdint.h>

typedef _Atomic uint32_t sp_mic_bank_generation_t;

/* Single producer. Advance even if this fill is never published. The fence
 * orders this marker before subsequent writes to the borrowed PCM storage. */
static inline uint32_t sp_mic_bank_write_start(sp_mic_bank_generation_t *bank)
{
    const uint32_t generation = atomic_load_explicit(bank, memory_order_relaxed) + 1u;
    atomic_store_explicit(bank, generation, memory_order_seq_cst);
    atomic_thread_fence(memory_order_seq_cst);
    return generation;
}

/* Use before and after sample access. The fences keep sample reads inside
 * the checks, including on SMP. These markers diagnose reuse, not pinning. */
static inline uint32_t sp_mic_bank_generation(const sp_mic_bank_generation_t *bank)
{
    atomic_thread_fence(memory_order_seq_cst);
    const uint32_t generation = atomic_load_explicit(bank, memory_order_seq_cst);
    atomic_thread_fence(memory_order_seq_cst);
    return generation;
}
