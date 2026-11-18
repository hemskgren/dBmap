/**
 * Host tests: AE signature capture buffer / CSV contract.
 *
 * From repo root:
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -g \
 *     -DCONFIG_DBMAP_EAR_AE_SIG_CAPTURE=1 \
 *     -I firmware/products/dbmap/ear/ae_runtime/include \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/acoustic_profile/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     -I firmware/platform/audio/include \
 *     firmware/products/dbmap/ear/acoustic_profile/as_profile_as001.c \
 *     firmware/products/dbmap/ear/acoustic_profile/as_profile_registry.c \
 *     firmware/products/dbmap/ear/acoustic_profile/as_profile_match.c \
 *     firmware/products/dbmap/ear/ae_runtime/ae_sig_capture.c \
 *     firmware/products/dbmap/ear/ae_runtime/test_host/test_ae_sig_capture.c \
 *     -lm -o /tmp/test_ae_sig_capture && /tmp/test_ae_sig_capture
 */
#include "ae_sig_capture.h"
#include "ae_types.h"
#include "as_profile.h"

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

static void fill_heli_like(ae_signature_t *s, uint32_t seq, float rms)
{
    memset(s, 0, sizeof(*s));
    s->schema_version = AE_SIGNATURE_SCHEMA_VERSION;
    s->window_seq = seq;
    s->window_end_mono_us = (int64_t)seq * 1000000;
    s->window_ms = 1000;
    s->mic_mask = 1u;
    s->quality = AE_QUALITY_OK;
    s->flags = AE_FLAG_PAIR_MISSING;
    s->rms_dbfs = rms;
    s->peak_dbfs = rms + 7.0f;
    s->crest_db = 7.0f;
    /* Approximate heli_peak relative bands. */
    s->band_dbfs[0] = rms - 8.2f;
    s->band_dbfs[1] = rms - 7.4f;
    s->band_dbfs[2] = rms - 16.9f;
    s->band_dbfs[3] = rms - 38.8f;
    s->hl_valid = true;
    s->hl_db = -12.0f;
    s->spectral_valid = true;
    s->centroid_hz = 1068.0f;
    s->bandwidth_hz = 1280.0f;
    s->dominant_hz = 500.0f;
    s->flatness = 0.25f;
    s->dynamics_valid = true;
}

static void test_compiled_and_header(void)
{
    EXPECT(ae_sig_capture_compiled());
    EXPECT(ae_sig_capture_capacity() == AE_SIG_CAPTURE_CAPACITY);
    EXPECT(strstr(AE_SIG_CSV_HEADER, "rms_dbfs") != NULL);
    EXPECT(strstr(AE_SIG_CSV_HEADER, "dominant_hz") != NULL);
    EXPECT(strstr(AE_SIG_CSV_HEADER, "as001_dist") != NULL);
    EXPECT(strstr(AE_SIG_CSV_PREFIX, "AE_SIG_CSV") != NULL);
}

static void test_settle_then_capture(void)
{
    ae_sig_capture_reset(42u);
    ae_sig_capture_diag_t d;
    ae_sig_capture_get_diag(&d);
    EXPECT(d.state == AE_SIG_CAPTURE_SETTLING || AE_SIG_CAPTURE_SETTLE_N == 0u);
    EXPECT(d.session_id == 42u);

    ae_signature_t sig;
    for (uint32_t i = 0; i < AE_SIG_CAPTURE_SETTLE_N; i++) {
        fill_heli_like(&sig, i + 1u, -30.0f);
        ae_sig_capture_on_signature(&sig);
    }
    ae_sig_capture_get_diag(&d);
    EXPECT(d.stored == 0u);

    fill_heli_like(&sig, 100u, -10.0f);
    ae_sig_capture_on_signature(&sig);
    ae_sig_capture_get_diag(&d);
    EXPECT(d.stored == 1u);
    EXPECT(d.state == AE_SIG_CAPTURE_ACTIVE || d.state == AE_SIG_CAPTURE_COMPLETE);
    EXPECT(ae_sig_capture_take_start_marker());
    EXPECT(!ae_sig_capture_take_start_marker());

    char line[512];
    EXPECT(ae_sig_capture_drain_one(line, sizeof(line)));
    EXPECT(strncmp(line, "42,100,", 7) == 0);
    /* channel column is 0 after sample_rate_hz=16000 */
    EXPECT(strstr(line, ",16000,0,") != NULL);

    as_profile_match_result_t mr;
    EXPECT(as_profile_match(as_profile_as001(), &sig, &mr));
    EXPECT(mr.matched);
}

static void test_overflow_reported(void)
{
    ae_sig_capture_reset(7u);
    ae_signature_t sig;
    /* Exhaust settle. */
    for (uint32_t i = 0; i < AE_SIG_CAPTURE_SETTLE_N; i++) {
        fill_heli_like(&sig, i + 1u, -40.0f);
        ae_sig_capture_on_signature(&sig);
    }
    for (uint32_t i = 0; i < AE_SIG_CAPTURE_CAPACITY; i++) {
        fill_heli_like(&sig, 1000u + i, -10.0f);
        ae_sig_capture_on_signature(&sig);
    }
    ae_sig_capture_diag_t d;
    ae_sig_capture_get_diag(&d);
    EXPECT(d.stored == AE_SIG_CAPTURE_CAPACITY);
    EXPECT(d.state == AE_SIG_CAPTURE_COMPLETE);
    EXPECT(d.overflow == 0u);

    fill_heli_like(&sig, 9999u, -10.0f);
    ae_sig_capture_on_signature(&sig);
    ae_sig_capture_get_diag(&d);
    EXPECT(d.overflow >= 1u);
    EXPECT(d.stored == AE_SIG_CAPTURE_CAPACITY);
}

static void test_format_csv_fields(void)
{
    ae_sig_capture_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.window_seq = 17;
    rec.window_end_mono_us = 17000000;
    rec.window_ms = 1000;
    rec.channel = 0;
    rec.quality = 0;
    rec.flags = 0x10;
    rec.rms_dbfs = -9.45f;
    rec.peak_dbfs = -2.11f;
    rec.crest_db = 7.34f;
    rec.band_dbfs[0] = -17.66f;
    rec.band_rel[0] = -8.21f;
    rec.spectral_valid = 1;
    rec.dominant_hz = 500.0f;
    rec.flatness = 0.246f;
    rec.as001_matched = 1;
    rec.as001_profile_id = AS_PROFILE_ID_AS001;
    rec.as001_dist = 0.627f;

    char line[512];
    EXPECT(ae_sig_capture_format_csv(&rec, 1u, line, sizeof(line)) > 0);
    EXPECT(strstr(line, "1,17,17000000,1000,16000,0,") != NULL);
    EXPECT(strstr(line, "-9.4500") != NULL);
    EXPECT(strstr(line, "500.0000") != NULL);
}

int main(void)
{
    test_compiled_and_header();
    test_settle_then_capture();
    test_overflow_reported();
    test_format_csv_fields();
    if (g_failures != 0) {
        fprintf(stderr, "test_ae_sig_capture: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ae_sig_capture: ok\n");
    return 0;
}
