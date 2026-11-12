/**
 * Host tests: AE signature build, history, multi-listener publish.
 *
 * From repo root:
 *   cc -std=c11 -Wall -Wextra -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     firmware/products/dbmap/ear/ear_acoustic/acoustic_l0.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_history.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_time.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_engine.c \
 *     firmware/products/dbmap/ear/audio_engine/test_host/test_ae_signature_history.c \
 *     -lm -o /tmp/test_ae_m1 && /tmp/test_ae_m1
 */
#include "ae_engine.h"
#include "ae_signature.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_listener_hits = 0;
static uint32_t g_listener_seq = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void fill_l0(dbmap_l0_window_t *l0, float rms, int64_t t_us)
{
    memset(l0, 0, sizeof(*l0));
    l0->valid = true;
    l0->window_end_mono_us = t_us;
    l0->window_ms = 1000;
    l0->sample_count = 16000;
    l0->rms_dbfs = rms;
    l0->peak_dbfs = rms + 6.0f;
    l0->crest_db = 6.0f;
    l0->band_dbfs[0] = rms - 3.0f;
    l0->band_dbfs[1] = rms;
    l0->band_dbfs[2] = rms - 1.0f;
    l0->band_dbfs[3] = rms - 6.0f;
    l0->hl_valid = true;
    l0->hl_db = 2.0f;
}

static void on_sig(const ae_signature_t *sig, void *ctx)
{
    (void)ctx;
    g_listener_hits++;
    g_listener_seq = sig->window_seq;
}

static void test_build_ok_and_pair(void)
{
    dbmap_l0_window_t l0;
    fill_l0(&l0, -40.0f, 1000000);
    dbmap_l1_window_t l1;
    memset(&l1, 0, sizeof(l1));
    l1.valid = true;
    l1.centroid_hz = 1000.0f;
    l1.bandwidth_hz = 400.0f;
    l1.dominant_hz = 880.0f;
    l1.flatness = 0.2f;

    ae_pair_metrics_t pair = {
        .valid = true,
        .mic0_rms_dbfs = -40.0f,
        .mic1_rms_dbfs = -46.0f,
        .mic0_peak_dbfs = -34.0f,
        .mic1_peak_dbfs = -40.0f,
        .mic_mask = 0x03u,
        .stereo_degraded = false,
    };

    ae_signature_t sig;
    EXPECT(ae_signature_build(7u, &l0, &l1, NULL, &pair, false, &sig));
    EXPECT(sig.schema_version == AE_SIGNATURE_SCHEMA_VERSION);
    EXPECT(sig.window_seq == 7u);
    EXPECT(sig.quality == AE_QUALITY_OK); /* L2 optional; L1 present */
    EXPECT(sig.pair_valid);
    EXPECT(fabsf(sig.delta_rms_db - 6.0f) < 0.01f);
    EXPECT((sig.flags & AE_FLAG_L2_MISSING) != 0u);
    EXPECT((sig.flags & AE_FLAG_L1_MISSING) == 0u);
}

static void test_gap_quality(void)
{
    dbmap_l0_window_t l0;
    fill_l0(&l0, -50.0f, 2000000);
    ae_signature_t sig;
    EXPECT(ae_signature_build(3u, &l0, NULL, NULL, NULL, true, &sig));
    EXPECT(sig.quality == AE_QUALITY_GAP);
}

static void test_history_and_listeners(void)
{
    ae_engine_t eng;
    ae_config_t cfg;
    ae_config_set_defaults(&cfg);
    EXPECT(ae_start(&eng, &cfg));
    EXPECT(ae_add_signature_listener(&eng, on_sig, NULL));

    dbmap_l0_window_t l0;
    for (uint32_t seq = 1; seq <= 5; seq++) {
        fill_l0(&l0, -45.0f - (float)seq, (int64_t)seq * 1000000);
        ae_signature_t sig;
        EXPECT(ae_signature_build(seq, &l0, NULL, NULL, NULL, false, &sig));
        EXPECT(ae_publish_signature(&eng, &sig));
    }

    EXPECT(g_listener_hits == 5);
    EXPECT(g_listener_seq == 5u);
    EXPECT(ae_last_window_seq(&eng) == 5u);

    ae_signature_t latest;
    EXPECT(ae_get_latest_signature(&eng, &latest));
    EXPECT(latest.window_seq == 5u);

    ae_signature_t buf[3];
    uint32_t n = ae_history_copy(&eng, buf, 3);
    EXPECT(n == 3u);
    EXPECT(buf[0].window_seq == 5u);
    EXPECT(buf[1].window_seq == 4u);
    EXPECT(buf[2].window_seq == 3u);
}

static void test_distance_similar(void)
{
    dbmap_l0_window_t a0, b0;
    fill_l0(&a0, -40.0f, 1);
    fill_l0(&b0, -41.0f, 2);
    ae_signature_t a, b;
    EXPECT(ae_signature_build(1u, &a0, NULL, NULL, NULL, false, &a));
    EXPECT(ae_signature_build(2u, &b0, NULL, NULL, NULL, false, &b));
    float d = ae_signature_distance(&a, &b);
    EXPECT(d < 4.0f);
    fill_l0(&b0, -70.0f, 3);
    EXPECT(ae_signature_build(3u, &b0, NULL, NULL, NULL, false, &b));
    EXPECT(ae_signature_distance(&a, &b) > 20.0f);
}

int main(void)
{
    test_build_ok_and_pair();
    test_gap_quality();
    test_history_and_listeners();
    test_distance_similar();
    if (g_failures != 0) {
        fprintf(stderr, "test_ae_signature_history: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ae_signature_history: ok\n");
    return 0;
}
