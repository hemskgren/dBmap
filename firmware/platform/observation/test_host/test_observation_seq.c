/**
 * Host unit tests for Observation sequence block allocation.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/platform/observation/include \
 *     firmware/platform/observation/observation_seq.c \
 *     firmware/platform/observation/test_host/test_observation_seq.c \
 *     -o /tmp/test_sp_observation_seq && /tmp/test_sp_observation_seq
 */
#include "sp_observation_seq.h"

#include <stdio.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void test_boot_skips_unused_remainder(void)
{
    sp_obs_seq_t seq;
    /* Durable limit 10 means 0..9 were reserved; reboot starts at 10. */
    sp_obs_seq_init(&seq, 10, 4);
    EXPECT(seq.next == 10);
    EXPECT(seq.limit == 10);
    EXPECT(sp_obs_seq_needs_reserve(&seq));

    uint32_t new_limit = sp_obs_seq_reserve(&seq);
    EXPECT(new_limit == 14);
    EXPECT(seq.limit == 14);
    EXPECT(!sp_obs_seq_needs_reserve(&seq));

    uint32_t a = 0, b = 0;
    EXPECT(sp_obs_seq_allocate(&seq, &a));
    EXPECT(a == 10);
    EXPECT(sp_obs_seq_allocate(&seq, &b));
    EXPECT(b == 11);
    /* 10 and 11 are consumed even if publish would fail afterward. */
}

static void test_allocate_requires_reserve(void)
{
    sp_obs_seq_t seq;
    sp_obs_seq_init(&seq, 0, 2);
    uint32_t n = 99;
    EXPECT(!sp_obs_seq_allocate(&seq, &n));
    EXPECT(n == 99);

    EXPECT(sp_obs_seq_reserve(&seq) == 2);
    EXPECT(sp_obs_seq_allocate(&seq, &n));
    EXPECT(n == 0);
    EXPECT(sp_obs_seq_allocate(&seq, &n));
    EXPECT(n == 1);
    EXPECT(!sp_obs_seq_allocate(&seq, &n));
    EXPECT(sp_obs_seq_needs_reserve(&seq));

    EXPECT(sp_obs_seq_reserve(&seq) == 4);
    EXPECT(sp_obs_seq_allocate(&seq, &n));
    EXPECT(n == 2);
}

static void test_never_reuses_after_partial_block(void)
{
    sp_obs_seq_t seq;
    sp_obs_seq_init(&seq, 0, 8);
    sp_obs_seq_reserve(&seq);
    uint32_t n;
    EXPECT(sp_obs_seq_allocate(&seq, &n));
    EXPECT(n == 0);
    EXPECT(sp_obs_seq_allocate(&seq, &n));
    EXPECT(n == 1);
    /* Simulate reboot with durable limit 8 (full prior block). */
    sp_obs_seq_init(&seq, 8, 8);
    sp_obs_seq_reserve(&seq);
    EXPECT(sp_obs_seq_allocate(&seq, &n));
    EXPECT(n == 8); /* 2..7 skipped — gap OK, never reuse 0/1 */
}

static void test_legacy_migration_skips_forward(void)
{
    /* Phase 1 seq=10 meant next to assign; migrate skips a full block. */
    EXPECT(sp_obs_seq_migrate_legacy_next(10, 64) == 74);
    EXPECT(sp_obs_seq_migrate_legacy_next(UINT32_MAX - 3, 64) == UINT32_MAX);

    sp_obs_seq_t seq;
    uint32_t lim = sp_obs_seq_migrate_legacy_next(5, 8);
    sp_obs_seq_init(&seq, lim, 8);
    sp_obs_seq_reserve(&seq);
    uint32_t n = 0;
    EXPECT(sp_obs_seq_allocate(&seq, &n));
    EXPECT(n == 13); /* never 0..12 */
}

static void test_overflow_stops_allocation(void)
{
    sp_obs_seq_t seq;
    sp_obs_seq_init(&seq, UINT32_MAX - 1, 8);
    EXPECT(sp_obs_seq_reserve(&seq) == UINT32_MAX);
    uint32_t n = 0;
    EXPECT(sp_obs_seq_allocate(&seq, &n));
    EXPECT(n == UINT32_MAX - 1);
    EXPECT(!sp_obs_seq_allocate(&seq, &n)); /* would wrap */
    EXPECT(sp_obs_seq_needs_reserve(&seq));
    EXPECT(sp_obs_seq_reserve(&seq) == UINT32_MAX);
    EXPECT(!sp_obs_seq_allocate(&seq, &n));
}

int main(void)
{
    test_boot_skips_unused_remainder();
    test_allocate_requires_reserve();
    test_never_reuses_after_partial_block();
    test_legacy_migration_skips_forward();
    test_overflow_stops_allocation();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
