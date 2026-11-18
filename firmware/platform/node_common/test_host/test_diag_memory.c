/**
 * Host tests for anomaly-only memory diagnostics.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *     -I firmware/platform/node_common/include \
 *     firmware/platform/node_common/diag_memory.c \
 *     firmware/platform/node_common/test_host/test_diag_memory.c \
 *     -o /tmp/test_diag_memory && /tmp/test_diag_memory
 */
#include "sp_diag_memory.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static sp_diag_mem_sample_t healthy_sample(void)
{
    sp_diag_mem_sample_t s;
    memset(&s, 0, sizeof(s));
    s.free_heap = 2u * 1024u * 1024u;
    s.min_free_heap = 1u * 1024u * 1024u;
    s.free_internal = 100u * 1024u;
    s.min_free_internal = 80u * 1024u;
    s.largest_internal = 40u * 1024u;
    s.stack_sampled = true;
    s.stack_hwm_min_bytes = 2048u;
    return s;
}

static void test_healthy_no_anomaly(void)
{
    sp_diag_mem_sample_t s = healthy_sample();
    sp_diag_mem_anomaly_t a;
    sp_diag_mem_evaluate(&s, &a);
    EXPECT(a.kinds == 0u);
    EXPECT(a.fingerprint == 0u);

    sp_diag_mem_reporter_t rep;
    sp_diag_mem_reporter_init(&rep);
    EXPECT(!sp_diag_mem_should_report(&rep, &a));
}

static void test_free_internal_anomaly_relevant_only(void)
{
    sp_diag_mem_sample_t s = healthy_sample();
    s.free_internal = SP_DIAG_MEM_FREE_INTERNAL_LOW_BYTES - 1u;
    sp_diag_mem_anomaly_t a;
    sp_diag_mem_evaluate(&s, &a);
    EXPECT(a.kinds & SP_DIAG_MEM_KIND_FREE_INTERNAL);
    EXPECT(!(a.kinds & SP_DIAG_MEM_KIND_FREE_HEAP));
    EXPECT(!(a.kinds & SP_DIAG_MEM_KIND_STACK_HWM));
    EXPECT(a.free_internal == s.free_internal);
}

static void test_fragmentation_includes_free_and_largest(void)
{
    sp_diag_mem_sample_t s = healthy_sample();
    s.free_internal = SP_DIAG_MEM_FRAG_FREE_INTERNAL_FLOOR_BYTES + 1024u;
    s.largest_internal = SP_DIAG_MEM_LARGEST_INTERNAL_LOW_BYTES - 1u;
    EXPECT(s.largest_internal < (s.free_internal / 2u));
    sp_diag_mem_anomaly_t a;
    sp_diag_mem_evaluate(&s, &a);
    EXPECT(a.kinds & SP_DIAG_MEM_KIND_FRAGMENTATION);
    EXPECT(!(a.kinds & SP_DIAG_MEM_KIND_FREE_INTERNAL));
}

static void test_largest_with_low_free_not_frag(void)
{
    sp_diag_mem_sample_t s = healthy_sample();
    s.free_internal = SP_DIAG_MEM_FREE_INTERNAL_LOW_BYTES - 100u;
    s.largest_internal = 1024u;
    sp_diag_mem_anomaly_t a;
    sp_diag_mem_evaluate(&s, &a);
    EXPECT(a.kinds & SP_DIAG_MEM_KIND_FREE_INTERNAL);
    EXPECT(a.kinds & SP_DIAG_MEM_KIND_LARGEST_INTERNAL);
    EXPECT(!(a.kinds & SP_DIAG_MEM_KIND_FRAGMENTATION));
}

static void test_stack_hwm_anomaly(void)
{
    sp_diag_mem_sample_t s = healthy_sample();
    s.stack_hwm_min_bytes = SP_DIAG_MEM_STACK_HWM_LOW_BYTES - 1u;
    sp_diag_mem_anomaly_t a;
    sp_diag_mem_evaluate(&s, &a);
    EXPECT(a.kinds & SP_DIAG_MEM_KIND_STACK_HWM);
}

static void test_stack_not_sampled_ignored(void)
{
    sp_diag_mem_sample_t s = healthy_sample();
    s.stack_sampled = false;
    s.stack_hwm_min_bytes = 1u;
    sp_diag_mem_anomaly_t a;
    sp_diag_mem_evaluate(&s, &a);
    EXPECT(!(a.kinds & SP_DIAG_MEM_KIND_STACK_HWM));
}

static void test_unchanged_anomaly_not_rereported(void)
{
    sp_diag_mem_sample_t s = healthy_sample();
    s.free_internal = 4096u;
    sp_diag_mem_anomaly_t a;
    sp_diag_mem_evaluate(&s, &a);
    EXPECT(a.kinds != 0u);

    sp_diag_mem_reporter_t rep;
    sp_diag_mem_reporter_init(&rep);
    EXPECT(sp_diag_mem_should_report(&rep, &a));
    sp_diag_mem_note_published(&rep, a.fingerprint);
    EXPECT(!sp_diag_mem_should_report(&rep, &a));

    /* Same bucket jitter still suppressed. */
    s.free_internal = 4096u + 100u;
    sp_diag_mem_evaluate(&s, &a);
    EXPECT(!sp_diag_mem_should_report(&rep, &a));
}

static void test_worsening_rereports(void)
{
    sp_diag_mem_sample_t s = healthy_sample();
    s.free_internal = 12u * 1024u;
    sp_diag_mem_anomaly_t a;
    sp_diag_mem_evaluate(&s, &a);

    sp_diag_mem_reporter_t rep;
    sp_diag_mem_reporter_init(&rep);
    EXPECT(sp_diag_mem_should_report(&rep, &a));
    sp_diag_mem_note_published(&rep, a.fingerprint);

    s.free_internal = 2u * 1024u; /* crosses 4 KiB quantum */
    sp_diag_mem_evaluate(&s, &a);
    EXPECT(sp_diag_mem_should_report(&rep, &a));
}

static void test_clear_allows_rereport(void)
{
    sp_diag_mem_sample_t s = healthy_sample();
    s.free_heap = SP_DIAG_MEM_FREE_HEAP_LOW_BYTES - 1u;
    sp_diag_mem_anomaly_t a;
    sp_diag_mem_evaluate(&s, &a);

    sp_diag_mem_reporter_t rep;
    sp_diag_mem_reporter_init(&rep);
    EXPECT(sp_diag_mem_should_report(&rep, &a));
    sp_diag_mem_note_published(&rep, a.fingerprint);
    EXPECT(!sp_diag_mem_should_report(&rep, &a));

    sp_diag_mem_sample_t ok = healthy_sample();
    sp_diag_mem_anomaly_t healthy;
    sp_diag_mem_evaluate(&ok, &healthy);
    EXPECT(!sp_diag_mem_should_report(&rep, &healthy));
    EXPECT(rep.last_reported_fp == 0u);

    sp_diag_mem_evaluate(&s, &a);
    EXPECT(sp_diag_mem_should_report(&rep, &a));
}

static void test_publish_fail_retries(void)
{
    sp_diag_mem_sample_t s = healthy_sample();
    s.min_free_internal = SP_DIAG_MEM_MIN_FREE_INTERNAL_LOW_BYTES - 1u;
    sp_diag_mem_anomaly_t a;
    sp_diag_mem_evaluate(&s, &a);

    sp_diag_mem_reporter_t rep;
    sp_diag_mem_reporter_init(&rep);
    EXPECT(sp_diag_mem_should_report(&rep, &a));
    /* Simulate failed publish: do not call note_published. */
    EXPECT(sp_diag_mem_should_report(&rep, &a));
}

static void test_min_free_heap_anomaly(void)
{
    sp_diag_mem_sample_t s = healthy_sample();
    s.min_free_heap = SP_DIAG_MEM_MIN_FREE_HEAP_LOW_BYTES - 1u;
    sp_diag_mem_anomaly_t a;
    sp_diag_mem_evaluate(&s, &a);
    EXPECT(a.kinds & SP_DIAG_MEM_KIND_MIN_FREE_HEAP);
    EXPECT(!(a.kinds & SP_DIAG_MEM_KIND_FREE_HEAP));
}

int main(void)
{
    test_healthy_no_anomaly();
    test_free_internal_anomaly_relevant_only();
    test_fragmentation_includes_free_and_largest();
    test_largest_with_low_free_not_frag();
    test_stack_hwm_anomaly();
    test_stack_not_sampled_ignored();
    test_unchanged_anomaly_not_rereported();
    test_worsening_rereports();
    test_clear_allows_rereport();
    test_publish_fail_retries();
    test_min_free_heap_anomaly();

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
