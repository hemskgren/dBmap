/**
 * NEW host tests: Iteration 1 background RMS rolling history.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g -lm \
 *     -I firmware/ear_components/ear_mic/include \
 *     firmware/ear_components/ear_mic/background_rms.c \
 *     firmware/ear_components/ear_mic/test_host/test_background_rms.c \
 *     -o /tmp/test_dbmap_background_rms && /tmp/test_dbmap_background_rms
 */
#include "dbmap_background_rms.h"

#include <math.h>
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

static dbmap_bg_rms_candidate_t ok_cand(uint32_t seq, uint32_t ms_en, float rms)
{
    dbmap_bg_rms_candidate_t c;
    memset(&c, 0, sizeof(c));
    c.levels_valid = true;
    c.rms_dbfs = rms;
    c.peak_dbfs = rms + 6.0f;
    c.clipped_samples = 0;
    c.window_seq = seq;
    c.ms_since_i2s_enable = ms_en;
    c.transport_status = "ok";
    c.microphone_status = "signal_present";
    return c;
}

static void test_linear_mean_energy(void)
{
    float samples[] = {-40.0f, -50.0f};
    float mean = 0.0f;
    EXPECT(dbmap_bg_rms_linear_mean_dbfs(samples, 2, &mean));
    /* mean_E = 0.5*(1e-4 + 1e-5) = 5.5e-5 → 10*log10 = -42.593... */
    EXPECT(fabs((double)mean - (-42.593358)) < 0.01);
    EXPECT(!dbmap_bg_rms_linear_mean_dbfs(samples, 0, &mean));
}

static void test_inclusion_rules(void)
{
    const uint32_t settle = 5000;
    dbmap_bg_rms_candidate_t c = ok_cand(2, 6000, -50.0f);
    EXPECT(dbmap_bg_rms_should_include(&c, settle));

    c = ok_cand(1, 6000, -50.0f);
    EXPECT(!dbmap_bg_rms_should_include(&c, settle));

    c = ok_cand(2, 1000, -50.0f);
    EXPECT(!dbmap_bg_rms_should_include(&c, settle));

    c = ok_cand(2, 6000, -50.0f);
    c.clipped_samples = 1;
    EXPECT(!dbmap_bg_rms_should_include(&c, settle));

    c = ok_cand(2, 6000, -50.0f);
    c.peak_dbfs = 0.0f;
    EXPECT(!dbmap_bg_rms_should_include(&c, settle));

    c = ok_cand(2, 6000, -50.0f);
    c.transport_status = "error";
    EXPECT(!dbmap_bg_rms_should_include(&c, settle));

    c = ok_cand(2, 6000, -50.0f);
    c.microphone_status = "noise_suspected";
    EXPECT(!dbmap_bg_rms_should_include(&c, settle));
}

static void test_snapshot_time_filter_read_only(void)
{
    dbmap_bg_rms_sample_t entries[8];
    dbmap_bg_rms_hist_t hist;
    dbmap_bg_rms_hist_init(&hist, entries, 8, 3000); /* 3 s window */

    dbmap_bg_rms_push(&hist, -50.0f, 1000000); /* 1 s */
    dbmap_bg_rms_push(&hist, -48.0f, 2000000);
    dbmap_bg_rms_push(&hist, -46.0f, 3000000);
    const uint32_t count_before = hist.count;

    /* Gap: no new inserts; at t=7s only sample at 3s? cutoff=7s-3s=4s → none in window. */
    dbmap_bg_rms_profile_t p;
    dbmap_bg_rms_profile(&hist, 7000000, &p);
    EXPECT(!p.present);
    EXPECT(hist.count == count_before); /* ring not mutated */

    /* At t=5s cutoff=2s → samples at 2s and 3s remain in filter. */
    dbmap_bg_rms_profile(&hist, 5000000, &p);
    EXPECT(p.present);
    EXPECT(p.sample_count == 2);
    EXPECT(hist.count == count_before);
}

static void test_partial_vs_full_elapsed(void)
{
    dbmap_bg_rms_sample_t entries[16];
    dbmap_bg_rms_hist_t hist;
    dbmap_bg_rms_hist_init(&hist, entries, 16, 5000); /* 5 s → full */

    dbmap_bg_rms_push(&hist, -55.0f, 1000000);
    dbmap_bg_rms_profile_t p;
    dbmap_bg_rms_profile(&hist, 2000000, &p);
    EXPECT(p.present);
    EXPECT(strcmp(p.status, "partial") == 0);
    EXPECT(p.sample_count == 1);

    dbmap_bg_rms_push(&hist, -54.0f, 3000000);
    dbmap_bg_rms_push(&hist, -53.0f, 6000000);
    dbmap_bg_rms_profile(&hist, 7000000, &p);
    EXPECT(p.present);
    EXPECT(strcmp(p.status, "full") == 0); /* elapsed 6s from first ≥ 5s */
    EXPECT(p.sample_count >= 1);
}

static void test_zero_samples_omits(void)
{
    dbmap_bg_rms_sample_t entries[4];
    dbmap_bg_rms_hist_t hist;
    dbmap_bg_rms_hist_init(&hist, entries, 4, 180000);
    dbmap_bg_rms_profile_t p;
    dbmap_bg_rms_profile(&hist, 1000, &p);
    EXPECT(!p.present);
}

int main(void)
{
    test_linear_mean_energy();
    test_inclusion_rules();
    test_snapshot_time_filter_read_only();
    test_partial_vs_full_elapsed();
    test_zero_samples_omits();

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
