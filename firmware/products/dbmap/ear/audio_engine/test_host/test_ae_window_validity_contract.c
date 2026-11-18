/**
 * Host tests: AE window timing, gaps, validity, and public consumer contract.
 *
 *   cc -std=c11 -Wall -Wextra -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     -I firmware/platform/audio/include \
 *     firmware/products/dbmap/ear/audio_engine/ae_align.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_gap.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_time.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_history.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_engine.c \
 *     firmware/products/dbmap/ear/audio_engine/test_host/test_ae_window_validity_contract.c \
 *     -lm -o /tmp/test_ae_window_validity_contract && /tmp/test_ae_window_validity_contract
 */
#include "ae_align.h"
#include "ae_engine.h"
#include "ae_gap.h"
#include "ae_signature.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_hits = 0;
static uint8_t g_last_quality = 0xffu;
static uint32_t g_last_seq = 0u;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void on_sig(const ae_signature_t *sig, void *ctx)
{
    (void)ctx;
    g_hits++;
    g_last_seq = sig->window_seq;
    g_last_quality = sig->quality;
}

static void fill_l0(dbmap_l0_window_t *l0, float rms, int64_t t_us, bool clipped)
{
    memset(l0, 0, sizeof(*l0));
    l0->valid = true;
    l0->window_end_mono_us = t_us;
    l0->window_ms = 1000;
    l0->sample_count = 16000;
    l0->rms_dbfs = rms;
    l0->peak_dbfs = clipped ? -0.01f : (rms + 6.0f);
    l0->crest_db = l0->peak_dbfs - l0->rms_dbfs;
    l0->clipped = clipped;
    l0->band_dbfs[0] = rms - 2.0f;
    l0->band_dbfs[1] = rms;
    l0->band_dbfs[2] = rms - 1.0f;
    l0->band_dbfs[3] = rms - 5.0f;
    l0->hl_valid = true;
    l0->hl_db = 1.5f;
}

static void fill_l1(dbmap_l1_window_t *l1, int64_t t_us)
{
    memset(l1, 0, sizeof(*l1));
    l1->valid = true;
    l1->window_end_mono_us = t_us;
    l1->window_ms = 1000;
    l1->centroid_hz = 1000.0f;
    l1->bandwidth_hz = 120.0f;
    l1->dominant_hz = 1000.0f;
    l1->flatness = 0.2f;
}

static bool publish_built(
    ae_engine_t *eng,
    uint32_t seq,
    int64_t t_us,
    bool gap_before,
    bool clipped,
    const ae_pair_metrics_t *pair
)
{
    dbmap_l0_window_t l0;
    fill_l0(&l0, -42.0f, t_us, clipped);
    dbmap_l1_window_t l1;
    fill_l1(&l1, t_us);
    ae_signature_t sig;
    if (!ae_signature_build(seq, &l0, &l1, NULL, pair, gap_before, &sig)) {
        return false;
    }
    return ae_publish_signature(eng, &sig);
}

static void test_timing_reject_and_recovery(void)
{
    ae_engine_t eng;
    ae_config_t cfg;
    ae_config_set_defaults(&cfg);
    EXPECT(ae_start(&eng, &cfg));
    g_hits = 0;
    EXPECT(ae_add_signature_listener(&eng, on_sig, NULL));

    EXPECT(publish_built(&eng, 1u, 1000000, false, false, NULL));
    EXPECT(g_hits == 1);
    EXPECT(g_last_seq == 1u);

    /* Non-monotonic time rejected; cursor unchanged. */
    EXPECT(!publish_built(&eng, 2u, 500000, false, false, NULL));
    EXPECT(ae_last_publish_reject_reason(&eng) == AE_TIME_NON_MONOTONIC);
    EXPECT(ae_last_window_seq(&eng) == 1u);
    EXPECT(g_hits == 1);

    /* Recovery: next valid seq + later timestamp publishes. */
    EXPECT(publish_built(&eng, 2u, 2000000, false, false, NULL));
    EXPECT(ae_last_window_seq(&eng) == 2u);
    EXPECT(ae_last_window_end_mono_us(&eng) == 2000000);
    EXPECT(g_hits == 2);
    EXPECT(g_last_seq == 2u);
}

static void test_gap_then_recovery(void)
{
    ae_engine_t eng;
    ae_config_t cfg;
    ae_config_set_defaults(&cfg);
    EXPECT(ae_start(&eng, &cfg));
    g_hits = 0;
    g_last_quality = 0xffu;
    EXPECT(ae_add_signature_listener(&eng, on_sig, NULL));

    uint32_t seen = 0u;
    ae_window_gap_t g0 =
        ae_window_gap_eval(ae_last_window_seq(&eng), 1u, 0u, &seen);
    EXPECT(!g0.gap_before);
    EXPECT(publish_built(&eng, 1u, 1000000, g0.gap_before, false, NULL));

    ae_window_gap_t g1 =
        ae_window_gap_eval(ae_last_window_seq(&eng), 4u, 0u, &seen);
    EXPECT(g1.seq_gap);
    EXPECT(g1.gap_before);
    EXPECT(publish_built(&eng, 4u, 4000000, g1.gap_before, false, NULL));
    EXPECT(g_last_quality == AE_QUALITY_GAP);

    ae_window_gap_t g2 =
        ae_window_gap_eval(ae_last_window_seq(&eng), 5u, 0u, &seen);
    EXPECT(!g2.gap_before);
    EXPECT(publish_built(&eng, 5u, 5000000, g2.gap_before, false, NULL));
    EXPECT(g_last_quality != AE_QUALITY_GAP);
    EXPECT(g_last_quality != AE_QUALITY_INVALID);
    EXPECT(g_hits == 3);
}

static void test_clip_and_usable_for_tracking(void)
{
    dbmap_l0_window_t l0;
    fill_l0(&l0, -20.0f, 9000000, true);
    dbmap_l1_window_t l1;
    fill_l1(&l1, 9000000);
    /* Clipped L1 is rejected by ae_signature_build spectral path. */
    l1.clipped = true;

    ae_signature_t sig;
    EXPECT(ae_signature_build(9u, &l0, &l1, NULL, NULL, false, &sig));
    EXPECT((sig.flags & AE_FLAG_CLIPPED) != 0u);
    EXPECT(sig.quality == AE_QUALITY_DEGRADED);
    /* Documented API: INVALID unusable; GAP still usable for coasting. */
    EXPECT(ae_signature_usable_for_tracking(&sig));

    ae_signature_t gap_sig;
    fill_l0(&l0, -40.0f, 10000000, false);
    EXPECT(ae_signature_build(10u, &l0, NULL, NULL, NULL, true, &gap_sig));
    EXPECT(gap_sig.quality == AE_QUALITY_GAP);
    EXPECT(ae_signature_usable_for_tracking(&gap_sig));

    fill_l0(&l0, -40.0f, 0, false);
    ae_signature_t inv;
    EXPECT(ae_signature_build(11u, &l0, NULL, NULL, NULL, false, &inv));
    EXPECT(inv.quality == AE_QUALITY_INVALID);
    EXPECT(!ae_signature_usable_for_tracking(&inv));
}

/**
 * Production gate used by ae_device_poll (ae_feature_matches_job_end).
 * Feature windows from another end time must not bind to this job.
 */
static void test_feature_job_end_consistency(void)
{
    const int64_t job_end = 12000000;
    EXPECT(ae_feature_matches_job_end(true, job_end, job_end));
    EXPECT(!ae_feature_matches_job_end(true, 11000000, job_end));
    EXPECT(!ae_feature_matches_job_end(false, job_end, job_end));

    dbmap_l0_window_t l0_a;
    fill_l0(&l0_a, -40.0f, 11000000, false);
    dbmap_l0_window_t l0_b;
    fill_l0(&l0_b, -55.0f, job_end, false);

    /* Device path: only matching L0 is passed to ae_signature_build. */
    const dbmap_l0_window_t *l0p = ae_feature_matches_job_end(
                                       l0_a.valid, l0_a.window_end_mono_us, job_end
                                   )
        ? &l0_a
        : NULL;
    EXPECT(l0p == NULL);

    l0p = ae_feature_matches_job_end(
              l0_b.valid, l0_b.window_end_mono_us, job_end
          )
        ? &l0_b
        : NULL;
    EXPECT(l0p == &l0_b);

    ae_signature_t sig;
    EXPECT(ae_signature_build(12u, l0p, NULL, NULL, NULL, false, &sig));
    EXPECT(sig.window_end_mono_us == job_end);
    EXPECT(fabsf(sig.rms_dbfs - (-55.0f)) < 0.01f);
    /* Stale window A levels must not appear under job B. */
    EXPECT(fabsf(sig.rms_dbfs - (-40.0f)) > 1.0f);
}

static void test_consumer_interface_fields(void)
{
    dbmap_l0_window_t l0;
    fill_l0(&l0, -35.0f, 15000000, false);
    dbmap_l1_window_t l1;
    fill_l1(&l1, 15000000);
    ae_pair_metrics_t pair = {
        .valid = true,
        .mic0_rms_dbfs = -35.0f,
        .mic1_rms_dbfs = -41.0f,
        .mic0_peak_dbfs = -29.0f,
        .mic1_peak_dbfs = -35.0f,
        .mic_mask = 0x03u,
        .stereo_degraded = false,
    };

    ae_signature_t sig;
    EXPECT(ae_signature_build(15u, &l0, &l1, NULL, &pair, false, &sig));

    EXPECT(sig.schema_version == AE_SIGNATURE_SCHEMA_VERSION);
    EXPECT(sig.window_seq == 15u);
    EXPECT(sig.window_end_mono_us == 15000000);
    EXPECT(sig.window_ms == 1000u);
    EXPECT(fabsf(sig.rms_dbfs - (-35.0f)) < 0.01f);
    EXPECT(fabsf(sig.peak_dbfs - (-29.0f)) < 0.01f);
    EXPECT(fabsf(sig.crest_db - 6.0f) < 0.01f);
    for (uint32_t i = 0; i < AE_BAND_COUNT; i++) {
        EXPECT(fabsf(sig.band_dbfs[i] - l0.band_dbfs[i]) < 0.01f);
    }
    EXPECT(sig.spectral_valid);
    EXPECT(fabsf(sig.dominant_hz - 1000.0f) < 0.01f);
    EXPECT(fabsf(sig.flatness - 0.2f) < 0.01f);
    EXPECT(sig.quality == AE_QUALITY_OK);
    EXPECT(sig.pair_valid);
    EXPECT(sig.mic_mask == 0x03u);
    EXPECT(fabsf(sig.delta_rms_db - 6.0f) < 0.01f);

    ae_engine_t eng;
    ae_config_t cfg;
    ae_config_set_defaults(&cfg);
    EXPECT(ae_start(&eng, &cfg));
    EXPECT(ae_publish_signature(&eng, &sig));
    ae_signature_t latest;
    EXPECT(ae_get_latest_signature(&eng, &latest));
    EXPECT(latest.window_seq == 15u);
    EXPECT(latest.schema_version == AE_SIGNATURE_SCHEMA_VERSION);
}

int main(void)
{
    test_timing_reject_and_recovery();
    test_gap_then_recovery();
    test_clip_and_usable_for_tracking();
    test_feature_job_end_consistency();
    test_consumer_interface_fields();
    if (g_failures != 0) {
        fprintf(
            stderr, "test_ae_window_validity_contract: %d failure(s)\n", g_failures
        );
        return 1;
    }
    printf("test_ae_window_validity_contract: ok\n");
    return 0;
}
