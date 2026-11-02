/**
 * Host unit tests for Observation sequence block allocation.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     firmware/ear_components/ear_observation/observation_seq.c \
 *     firmware/ear_components/ear_observation/test_host/test_observation_seq.c \
 *     -o /tmp/test_dbmap_observation_seq && /tmp/test_dbmap_observation_seq
 */
#include "dbmap_observation_seq.h"

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
    dbmap_obs_seq_t seq;
    /* Durable limit 10 means 0..9 were reserved; reboot starts at 10. */
    dbmap_obs_seq_init(&seq, 10, 4);
    EXPECT(seq.next == 10);
    EXPECT(seq.limit == 10);
    EXPECT(dbmap_obs_seq_needs_reserve(&seq));

    uint32_t new_limit = dbmap_obs_seq_reserve(&seq);
    EXPECT(new_limit == 14);
    EXPECT(seq.limit == 14);
    EXPECT(!dbmap_obs_seq_needs_reserve(&seq));

    uint32_t a = 0, b = 0;
    EXPECT(dbmap_obs_seq_allocate(&seq, &a));
    EXPECT(a == 10);
    EXPECT(dbmap_obs_seq_allocate(&seq, &b));
    EXPECT(b == 11);
    /* 10 and 11 are consumed even if publish would fail afterward. */
}

static void test_allocate_requires_reserve(void)
{
    dbmap_obs_seq_t seq;
    dbmap_obs_seq_init(&seq, 0, 2);
    uint32_t n = 99;
    EXPECT(!dbmap_obs_seq_allocate(&seq, &n));
    EXPECT(n == 99);

    EXPECT(dbmap_obs_seq_reserve(&seq) == 2);
    EXPECT(dbmap_obs_seq_allocate(&seq, &n));
    EXPECT(n == 0);
    EXPECT(dbmap_obs_seq_allocate(&seq, &n));
    EXPECT(n == 1);
    EXPECT(!dbmap_obs_seq_allocate(&seq, &n));
    EXPECT(dbmap_obs_seq_needs_reserve(&seq));

    EXPECT(dbmap_obs_seq_reserve(&seq) == 4);
    EXPECT(dbmap_obs_seq_allocate(&seq, &n));
    EXPECT(n == 2);
}

static void test_never_reuses_after_partial_block(void)
{
    dbmap_obs_seq_t seq;
    dbmap_obs_seq_init(&seq, 0, 8);
    dbmap_obs_seq_reserve(&seq);
    uint32_t n;
    EXPECT(dbmap_obs_seq_allocate(&seq, &n));
    EXPECT(n == 0);
    EXPECT(dbmap_obs_seq_allocate(&seq, &n));
    EXPECT(n == 1);
    /* Simulate reboot with durable limit 8 (full prior block). */
    dbmap_obs_seq_init(&seq, 8, 8);
    dbmap_obs_seq_reserve(&seq);
    EXPECT(dbmap_obs_seq_allocate(&seq, &n));
    EXPECT(n == 8); /* 2..7 skipped — gap OK, never reuse 0/1 */
}

static void test_legacy_migration_skips_forward(void)
{
    /* Phase 1 seq=10 meant next to assign; migrate skips a full block. */
    EXPECT(dbmap_obs_seq_migrate_legacy_next(10, 64) == 74);
    EXPECT(dbmap_obs_seq_migrate_legacy_next(UINT32_MAX - 3, 64) == UINT32_MAX);

    dbmap_obs_seq_t seq;
    uint32_t lim = dbmap_obs_seq_migrate_legacy_next(5, 8);
    dbmap_obs_seq_init(&seq, lim, 8);
    dbmap_obs_seq_reserve(&seq);
    uint32_t n = 0;
    EXPECT(dbmap_obs_seq_allocate(&seq, &n));
    EXPECT(n == 13); /* never 0..12 */
}

static void test_overflow_stops_allocation(void)
{
    dbmap_obs_seq_t seq;
    dbmap_obs_seq_init(&seq, UINT32_MAX - 1, 8);
    EXPECT(dbmap_obs_seq_reserve(&seq) == UINT32_MAX);
    uint32_t n = 0;
    EXPECT(dbmap_obs_seq_allocate(&seq, &n));
    EXPECT(n == UINT32_MAX - 1);
    EXPECT(!dbmap_obs_seq_allocate(&seq, &n)); /* would wrap */
    EXPECT(dbmap_obs_seq_needs_reserve(&seq));
    EXPECT(dbmap_obs_seq_reserve(&seq) == UINT32_MAX);
    EXPECT(!dbmap_obs_seq_allocate(&seq, &n));
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
