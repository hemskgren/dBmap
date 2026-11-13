/**
 * Host tests: AE stereo foundation validity (no TDOA).
 *
 *   cc -std=c11 -Wall -Wextra -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     firmware/products/dbmap/ear/audio_engine/ae_align.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_stereo.c \
 *     firmware/products/dbmap/ear/audio_engine/test_host/test_ae_stereo.c \
 *     -o /tmp/test_ae_stereo && /tmp/test_ae_stereo
 */
#include "ae_align.h"
#include "ae_stereo.h"

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

static ae_align_result_t make_pair_ok(bool degraded)
{
    ae_align_result_t r;
    memset(&r, 0, sizeof(r));
    r.mic0_aligned = true;
    r.mic1_aligned = true;
    r.pair_aligned = true;
    r.pair.valid = true;
    r.pair.mic0_rms_dbfs = -40.0f;
    r.pair.mic1_rms_dbfs = -46.0f;
    r.pair.mic_mask = 0x03u;
    r.pair.stereo_degraded = degraded;
    return r;
}

static void test_stereo_ok(void)
{
    ae_align_result_t a = make_pair_ok(false);
    ae_stereo_eval_t e = ae_stereo_eval(&a, true, 0x03u, 16000, 16000);
    EXPECT(e.state == AE_STEREO_OK);
    EXPECT(e.pair_metrics_ok);
}

static void test_pcm_incomplete(void)
{
    ae_align_result_t a = make_pair_ok(false);
    ae_stereo_eval_t e = ae_stereo_eval(&a, true, 0x01u, 16000, 0);
    EXPECT(e.state == AE_STEREO_PCM_INCOMPLETE);
}

static void test_misaligned(void)
{
    ae_align_result_t a;
    memset(&a, 0, sizeof(a));
    a.mic0_aligned = true;
    a.mismatch_flags = AE_ALIGN_MISMATCH_MIC1;
    ae_stereo_eval_t e = ae_stereo_eval(&a, true, 0x03u, 16000, 16000);
    EXPECT(e.state == AE_STEREO_MISALIGNED);
}

static void test_mono_fallback(void)
{
    ae_align_result_t a;
    memset(&a, 0, sizeof(a));
    a.mic0_aligned = true;
    ae_stereo_eval_t e = ae_stereo_eval(&a, true, 0x01u, 16000, 0);
    EXPECT(e.state == AE_STEREO_MONO_FALLBACK);
}

static void test_none_when_mono_expected(void)
{
    ae_align_result_t a = make_pair_ok(false);
    ae_stereo_eval_t e = ae_stereo_eval(&a, false, 0x01u, 16000, 0);
    EXPECT(e.state == AE_STEREO_NONE);
}

static void test_degraded(void)
{
    ae_align_result_t a = make_pair_ok(true);
    ae_stereo_eval_t e = ae_stereo_eval(&a, true, 0x03u, 16000, 16000);
    EXPECT(e.state == AE_STEREO_DEGRADED);
}

int main(void)
{
    test_stereo_ok();
    test_pcm_incomplete();
    test_misaligned();
    test_mono_fallback();
    test_none_when_mono_expected();
    test_degraded();
    if (g_failures != 0) {
        fprintf(stderr, "test_ae_stereo: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ae_stereo: ok\n");
    return 0;
}
