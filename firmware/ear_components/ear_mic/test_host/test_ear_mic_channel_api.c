/**
 * Host unit tests for Stage 1 ear mic channel/stream API fillers.
 *
 * Build/run from repo root:
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_mic/include \
 *     firmware/ear_components/ear_mic/ear_mic_channel_api.c \
 *     firmware/ear_components/ear_mic/audio_metrics.c \
 *     firmware/ear_components/ear_mic/test_host/test_ear_mic_channel_api.c \
 *     -lm -o /tmp/test_dbmap_ear_mic_channel_api && /tmp/test_dbmap_ear_mic_channel_api
 *
 * These tests exercise the same fill helpers used by get_channel / get_levels /
 * get_stream on device. They do not link ear_mic.c (ESP-IDF).
 */
#include "dbmap_ear_mic_channel_api.h"

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

#define EXPECT_NEAR(actual, expected, tol)                                     \
    do {                                                                       \
        double _a = (double)(actual);                                          \
        double _e = (double)(expected);                                        \
        if (fabs(_a - _e) > (tol)) {                                           \
            fprintf(                                                           \
                stderr,                                                        \
                "FAIL %s:%d: %s=%g expected %g ±%g\n",                         \
                __FILE__,                                                      \
                __LINE__,                                                      \
                #actual,                                                       \
                _a,                                                            \
                _e,                                                            \
                (double)(tol)                                                  \
            );                                                                 \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void fill_view(
    dbmap_ear_mic_window_view_t *v,
    float rms,
    float peak,
    uint32_t n,
    uint32_t seq,
    int64_t measured_at
)
{
    memset(v, 0, sizeof(*v));
    v->rms_dbfs = rms;
    v->peak_dbfs = peak;
    v->sample_count = n;
    v->window_ms = 1000;
    v->window_seq = seq;
    v->ms_since_i2s_enable = 5000;
    v->measured_at_us = measured_at;
    v->zero_sample_count = 0;
    v->clipped_samples = 0;
    strncpy(v->channel_presence, "active", sizeof(v->channel_presence) - 1);
    strncpy(v->transport_status, "ok", sizeof(v->transport_status) - 1);
    strncpy(v->microphone_status, "signal_present", sizeof(v->microphone_status) - 1);
    strncpy(v->classification_reason, "ambient_like_energy", sizeof(v->classification_reason) - 1);
    v->raw_min = -100;
    v->raw_max = 200;
    v->read_calls = 10;
    v->bytes_received = 4096;
    v->complete_frames = n;
}

static void test_channel0_matches_primary_levels(void)
{
    dbmap_ear_mic_window_view_t view;
    fill_view(&view, -42.5f, -20.0f, 16000, 17, 123456789LL);

    dbmap_ear_mic_channel_t ch;
    dbmap_ear_mic_primary_levels_t levels;
    EXPECT(dbmap_ear_mic_fill_channel(0, 2, &view, 16000, &ch) == DBMAP_EAR_MIC_API_OK);
    EXPECT(dbmap_ear_mic_fill_primary_levels(&view, &levels) == DBMAP_EAR_MIC_API_OK);

    EXPECT(ch.valid == true);
    EXPECT(levels.valid == true);
    EXPECT_NEAR(ch.rms_dbfs, levels.rms_dbfs, 1e-6);
    EXPECT_NEAR(ch.peak_dbfs, levels.peak_dbfs, 1e-6);
    EXPECT(ch.window_seq == levels.window_seq);
    EXPECT(ch.window_ms == levels.window_ms);
    EXPECT(ch.measured_at_us == levels.measured_at_us);
    EXPECT(ch.ms_since_i2s_enable == levels.ms_since_i2s_enable);
    EXPECT(strcmp(ch.transport_status, levels.transport_status) == 0);
    EXPECT(strcmp(ch.microphone_status, levels.microphone_status) == 0);
    EXPECT(ch.index == 0);
    EXPECT(strcmp(ch.slot, "left") == 0);
    EXPECT(ch.sample_count == 16000);
    EXPECT(ch.expected_sample_count == 16000);
    EXPECT(strcmp(ch.channel_presence, "active") == 0);
    EXPECT(strcmp(ch.classification_reason, "ambient_like_energy") == 0);
    EXPECT(ch.raw_min == -100);
    EXPECT(ch.raw_max == 200);
}

static void test_channel1_accessible_when_stereo(void)
{
    dbmap_ear_mic_window_view_t view;
    fill_view(&view, -55.0f, -30.0f, 15990, 17, 123456789LL);

    dbmap_ear_mic_channel_t ch;
    EXPECT(dbmap_ear_mic_fill_channel(1, 2, &view, 16000, &ch) == DBMAP_EAR_MIC_API_OK);
    EXPECT(ch.index == 1);
    EXPECT(strcmp(ch.slot, "right") == 0);
    EXPECT(ch.valid == true);
    EXPECT_NEAR(ch.rms_dbfs, -55.0, 1e-6);
    EXPECT(ch.window_seq == 17);
    EXPECT(ch.sample_count == 15990);
}

static void test_out_of_range_index_fails(void)
{
    dbmap_ear_mic_window_view_t view;
    fill_view(&view, -40.0f, -20.0f, 16000, 3, 1);

    dbmap_ear_mic_channel_t ch;
    EXPECT(dbmap_ear_mic_fill_channel(1, 1, &view, 16000, &ch) == DBMAP_EAR_MIC_API_ERR_INVALID_ARG);
    EXPECT(dbmap_ear_mic_fill_channel(2, 2, &view, 16000, &ch) == DBMAP_EAR_MIC_API_ERR_INVALID_ARG);
    EXPECT(dbmap_ear_mic_fill_channel(0, 0, &view, 16000, &ch) == DBMAP_EAR_MIC_API_ERR_INVALID_ARG);
    EXPECT(dbmap_ear_mic_fill_channel(0, 2, NULL, 16000, &ch) == DBMAP_EAR_MIC_API_ERR_INVALID_ARG);
    EXPECT(dbmap_ear_mic_fill_channel(0, 2, &view, 16000, NULL) == DBMAP_EAR_MIC_API_ERR_INVALID_ARG);
}

static void test_stream_metadata_and_common_seq(void)
{
    dbmap_ear_mic_window_view_t views[2];
    fill_view(&views[0], -40.0f, -18.0f, 16000, 42, 999);
    fill_view(&views[1], -41.0f, -19.0f, 16000, 42, 999);

    dbmap_ear_mic_stream_t stream;
    EXPECT(
        dbmap_ear_mic_fill_stream(2, 2, views, "stereo_lr", 0, &stream)
        == DBMAP_EAR_MIC_API_OK
    );
    EXPECT(stream.live_mic_count == 2);
    EXPECT(stream.configured_mic_count == 2);
    EXPECT(stream.available_mic_count == 2);
    EXPECT(stream.window_seq == 42);
    EXPECT(stream.incomplete_stereo_frames == 0);
    EXPECT(strcmp(stream.slot_mode, "stereo_lr") == 0);

    /* Open-SD style: samples still present (silence) → still available. */
    views[1].rms_dbfs = -120.0f;
    views[1].peak_dbfs = -120.0f;
    views[1].zero_sample_count = 16000;
    views[1].all_zero_window = 1;
    strncpy(views[1].channel_presence, "silent", sizeof(views[1].channel_presence) - 1);
    strncpy(
        views[1].microphone_status,
        "disconnected_suspected",
        sizeof(views[1].microphone_status) - 1
    );
    EXPECT(
        dbmap_ear_mic_fill_stream(2, 2, views, "stereo_lr", 3, &stream)
        == DBMAP_EAR_MIC_API_OK
    );
    EXPECT(stream.available_mic_count == 2);
    EXPECT(stream.incomplete_stereo_frames == 3);
    EXPECT(stream.window_seq == 42);

    /* True missing_data / zero samples reduces available count. */
    views[1].sample_count = 0;
    views[1].missing_data = 1;
    EXPECT(
        dbmap_ear_mic_fill_stream(2, 2, views, "stereo_lr", 0, &stream)
        == DBMAP_EAR_MIC_API_OK
    );
    EXPECT(stream.available_mic_count == 1);

    EXPECT(dbmap_ear_mic_fill_stream(2, 2, NULL, "stereo_lr", 0, &stream)
           == DBMAP_EAR_MIC_API_ERR_INVALID_ARG);
    EXPECT(dbmap_ear_mic_fill_stream(0, 2, views, "stereo_lr", 0, &stream)
           == DBMAP_EAR_MIC_API_ERR_INVALID_ARG);
}

static void test_unmeasured_window_not_valid(void)
{
    dbmap_ear_mic_window_view_t view;
    memset(&view, 0, sizeof(view));
    view.rms_dbfs = -120.0f;
    view.peak_dbfs = -120.0f;

    dbmap_ear_mic_channel_t ch;
    dbmap_ear_mic_primary_levels_t levels;
    EXPECT(dbmap_ear_mic_fill_channel(0, 1, &view, 16000, &ch) == DBMAP_EAR_MIC_API_OK);
    EXPECT(dbmap_ear_mic_fill_primary_levels(&view, &levels) == DBMAP_EAR_MIC_API_OK);
    EXPECT(ch.valid == false);
    EXPECT(levels.valid == false);
    EXPECT(ch.window_seq == levels.window_seq);
}

int main(void)
{
    test_channel0_matches_primary_levels();
    test_channel1_accessible_when_stereo();
    test_out_of_range_index_fails();
    test_stream_metadata_and_common_seq();
    test_unmeasured_window_not_valid();

    if (g_failures != 0) {
        fprintf(stderr, "test_ear_mic_channel_api: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ear_mic_channel_api: ok\n");
    return 0;
}
