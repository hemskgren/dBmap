/**
 * Host tests: AE acquisition-time contract, publish rejects, history expiry.
 *
 *   cc -std=c11 -Wall -Wextra -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     firmware/products/dbmap/ear/ear_acoustic/acoustic_l0.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_history.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_time.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_engine.c \
 *     firmware/products/dbmap/ear/audio_engine/test_host/test_ae_time_contract.c \
 *     -lm -o /tmp/test_ae_time && /tmp/test_ae_time
 */
#include "ae_contract.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_hits = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void on_sig(const ae_signature_t *sig, void *ctx)
{
    (void)sig;
    (void)ctx;
    g_hits++;
}

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
}

static bool build_and_try_publish(
    ae_engine_t *eng,
    uint32_t seq,
    int64_t t_us,
    float rms
)
{
    dbmap_l0_window_t l0;
    fill_l0(&l0, rms, t_us);
    ae_signature_t sig;
    if (!ae_signature_build(seq, &l0, NULL, NULL, NULL, false, &sig)) {
        return false;
    }
    return ae_publish_signature(eng, &sig);
}

static void test_missing_and_invalid_time(void)
{
    ae_engine_t eng;
    ae_config_t cfg;
    ae_config_set_defaults(&cfg);
    EXPECT(ae_start(&eng, &cfg));

    dbmap_l0_window_t l0;
    fill_l0(&l0, -40.0f, 0);
    ae_signature_t sig;
    EXPECT(ae_signature_build(1u, &l0, NULL, NULL, NULL, false, &sig));
    EXPECT(sig.quality == AE_QUALITY_INVALID);
    EXPECT(!ae_publish_signature(&eng, &sig));
    EXPECT(ae_last_publish_reject_reason(&eng) == AE_TIME_MISSING);
    EXPECT(ae_publish_rejected_count(&eng) == 1u);
}

static void test_monotonic_and_duplicate_seq(void)
{
    ae_engine_t eng;
    ae_config_t cfg;
    ae_config_set_defaults(&cfg);
    EXPECT(ae_start(&eng, &cfg));
    EXPECT(ae_add_signature_listener(&eng, on_sig, NULL));
    g_hits = 0;

    EXPECT(build_and_try_publish(&eng, 1u, 1000000, -40.0f));
    EXPECT(build_and_try_publish(&eng, 2u, 2000000, -41.0f));
    EXPECT(g_hits == 2);

    /* Non-monotonic time */
    EXPECT(!build_and_try_publish(&eng, 3u, 1500000, -42.0f));
    EXPECT(ae_last_publish_reject_reason(&eng) == AE_TIME_NON_MONOTONIC);

    /* Duplicate seq */
    EXPECT(!build_and_try_publish(&eng, 2u, 3000000, -42.0f));
    EXPECT(ae_last_publish_reject_reason(&eng) == AE_TIME_DUPLICATE_SEQ);

    /* Seq regress */
    EXPECT(!build_and_try_publish(&eng, 1u, 3000000, -42.0f));
    EXPECT(ae_last_publish_reject_reason(&eng) == AE_TIME_SEQ_REGRESS);

    /* Forward gap OK */
    EXPECT(build_and_try_publish(&eng, 5u, 3000000, -42.0f));
    EXPECT(ae_last_window_seq(&eng) == 5u);
    EXPECT(ae_last_window_end_mono_us(&eng) == 3000000);
    EXPECT(g_hits == 3);
    EXPECT(ae_publish_rejected_count(&eng) == 3u);
}

static void test_history_since_and_expire(void)
{
    ae_engine_t eng;
    ae_config_t cfg;
    ae_config_set_defaults(&cfg);
    EXPECT(ae_start(&eng, &cfg));

    for (uint32_t seq = 1; seq <= 5; seq++) {
        EXPECT(build_and_try_publish(
            &eng, seq, (int64_t)seq * 1000000, -40.0f - (float)seq
        ));
    }

    ae_signature_t buf[8];
    uint32_t n = ae_history_copy_since(&eng, 3000000, buf, 8);
    EXPECT(n == 3u);
    EXPECT(buf[0].window_seq == 5u);
    EXPECT(buf[1].window_seq == 4u);
    EXPECT(buf[2].window_seq == 3u);

    uint32_t removed = ae_history_expire_before_mono(&eng, 4000000);
    EXPECT(removed == 3u);
    n = ae_history_copy(&eng, buf, 8);
    EXPECT(n == 2u);
    EXPECT(buf[0].window_seq == 5u);
    EXPECT(buf[1].window_seq == 4u);
    /* Cursor still reflects last accepted publish (not rewritten by expire). */
    EXPECT(ae_last_window_seq(&eng) == 5u);
    EXPECT(ae_last_window_end_mono_us(&eng) == 5000000);
}

static void test_stereo_contract_identity_not_tdoa(void)
{
    /* Documented guarantee: pair metrics encode channel identity, not phase. */
    ae_pair_metrics_t pair = {
        .valid = true,
        .mic0_rms_dbfs = -30.0f,
        .mic1_rms_dbfs = -36.0f,
        .mic0_peak_dbfs = -24.0f,
        .mic1_peak_dbfs = -30.0f,
        .mic_mask = 0x03u,
        .stereo_degraded = false,
    };
    dbmap_l0_window_t l0;
    fill_l0(&l0, -30.0f, 9000000);
    ae_signature_t sig;
    EXPECT(ae_signature_build(9u, &l0, NULL, NULL, &pair, false, &sig));
    EXPECT(sig.pair_valid);
    EXPECT(sig.mic_mask == 0x03u);
    EXPECT(sig.window_end_mono_us == 9000000);
    EXPECT((sig.mic_mask & 0x01u) != 0u); /* mic0 identity bit */
    EXPECT((sig.mic_mask & 0x02u) != 0u); /* mic1 identity bit */
}

int main(void)
{
    test_missing_and_invalid_time();
    test_monotonic_and_duplicate_seq();
    test_history_since_and_expire();
    test_stereo_contract_identity_not_tdoa();
    if (g_failures != 0) {
        fprintf(stderr, "test_ae_time_contract: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ae_time_contract: ok\n");
    return 0;
}
