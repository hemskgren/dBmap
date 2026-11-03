/**
 * Host unit tests for hierarchical Observation JSON.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     -I firmware/ear_components/ear_mic/include \
 *     -I $IDF_PATH/components/json/cJSON \
 *     firmware/ear_components/ear_observation/observation_payload.c \
 *     firmware/ear_components/ear_mic/acoustic_l0.c \
 *     $IDF_PATH/components/json/cJSON/cJSON.c \
 *     firmware/ear_components/ear_observation/test_host/test_observation_payload.c \
 *     -lm -o /tmp/test_observation_payload && /tmp/test_observation_payload
 *
 * If IDF cJSON is unavailable, link against system libcjson.
 */
#include "dbmap_observation_payload.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static dbmap_l0_interval_t empty_interval(void)
{
    dbmap_l0_interval_t iv;
    memset(&iv, 0, sizeof(iv));
    iv.sample_rate_hz = 16000;
    iv.analysis_window_ms = 1000;
    return iv;
}

static dbmap_l0_interval_t sample_interval(void)
{
    dbmap_l0_interval_t iv = empty_interval();
    iv.window_count = 5;
    iv.clipped_window_count = 1;
    iv.n_crest = 4;
    iv.has_rms_stats = true;
    iv.rms_mean_dbfs = -42.5f;
    iv.rms_min_dbfs = -50.0f;
    iv.rms_max_dbfs = -35.0f;
    iv.has_band_means = true;
    iv.band_mean_dbfs[0] = -40.0f;
    iv.band_mean_dbfs[1] = -41.0f;
    iv.band_mean_dbfs[2] = -42.0f;
    iv.band_mean_dbfs[3] = -43.0f;
    iv.has_band_fractions = true;
    iv.band_fraction[0] = 0.4f;
    iv.band_fraction[1] = 0.3f;
    iv.band_fraction[2] = 0.2f;
    iv.band_fraction[3] = 0.1f;
    iv.has_hl_mean = true;
    iv.hl_mean_db = -3.0f;
    iv.has_t1 = true;
    iv.rms_stdev_db = 2.0f;
    iv.rms_range_db = 15.0f;
    iv.has_t2 = true;
    iv.rms_slope_db_per_s = 0.25f;
    iv.has_t3 = true;
    iv.crest_max_db = 14.0f;
    iv.high_crest_count = 1;
    iv.high_crest_rate = 0.25f;
    iv.has_t6 = true;
    iv.hl_slope_db_per_s = -0.1f;
    return iv;
}

static void test_hierarchical_start(void)
{
    dbmap_l0_interval_t iv = sample_interval();
    dbmap_observation_fields_t fields = {
        .observation_id = "11111111-2222-4333-8444-555555555555",
        .node_id = "EAR-001",
        .hub_id = "HUB-001",
        .sequence_number = 7,
        .event_time_utc = "2026-11-03T12:00:00Z",
        .capture_timestamp_monotonic_us = 2000000,
        .timing_quality = "estimated",
        .has_clock_offset_ms = false,
        .timestamp_uncertainty_ms = 25.0,
        .interval_duration_ms = 2000.0,
        .episode_duration_ms = 2000.0,
        .emission_kind = "start",
        .termination_reason = NULL,
        .has_signal_level_dbfs = true,
        .signal_level_dbfs = -42.5,
        .measurements = &iv,
    };
    char *json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL);
    if (json) {
        EXPECT(strstr(json, "\"protocol_version\":1") != NULL);
        EXPECT(strstr(json, "\"identity\"") != NULL);
        EXPECT(strstr(json, "\"hub_id\":\"HUB-001\"") != NULL);
        EXPECT(strstr(json, "\"interval_duration_ms\":2000") != NULL);
        EXPECT(strstr(json, "\"episode_duration_ms\":2000") != NULL);
        EXPECT(strstr(json, "\"emission_kind\":\"start\"") != NULL);
        EXPECT(strstr(json, "termination_reason") == NULL);
        EXPECT(strstr(json, "\"signature\":null") != NULL);
        EXPECT(strstr(json, "\"classification\":null") != NULL);
        EXPECT(strstr(json, "\"bearing\":null") != NULL);
        free(json);
    }
}

static void test_complete_requires_termination(void)
{
    dbmap_l0_interval_t iv = empty_interval();
    dbmap_observation_fields_t fields = {
        .observation_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
        .node_id = "EAR-001",
        .hub_id = "HUB-001",
        .sequence_number = 1,
        .event_time_utc = "2026-11-03T12:00:00Z",
        .capture_timestamp_monotonic_us = 1000000,
        .timing_quality = "estimated",
        .timestamp_uncertainty_ms = 10.0,
        .interval_duration_ms = 1000.0,
        .episode_duration_ms = 1000.0,
        .emission_kind = "complete",
        .termination_reason = NULL,
        .has_signal_level_dbfs = false,
        .signal_level_dbfs = 0.0,
        .measurements = &iv,
    };
    EXPECT(dbmap_observation_build_json(&fields) == NULL);
    fields.termination_reason = "natural";
    char *json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL);
    if (json) {
        EXPECT(strstr(json, "\"emission_kind\":\"complete\"") != NULL);
        EXPECT(strstr(json, "\"termination_reason\":\"natural\"") != NULL);
        EXPECT(strstr(json, "\"signal_level_dbfs\":null") != NULL);
        EXPECT(strstr(json, "\"window_count\":0") != NULL);
        EXPECT(strstr(json, "\"mean_dbfs\":null") != NULL);
        free(json);
    }
}

static void test_end_empty_l0(void)
{
    dbmap_l0_interval_t iv = empty_interval();
    dbmap_observation_fields_t fields = {
        .observation_id = "11111111-2222-4333-8444-555555555555",
        .node_id = "EAR-001",
        .hub_id = "",
        .sequence_number = 3,
        .event_time_utc = "2026-11-03T12:00:00Z",
        .capture_timestamp_monotonic_us = 1,
        .timing_quality = "synchronized",
        .timestamp_uncertainty_ms = 10.0,
        .interval_duration_ms = 1000.0,
        .episode_duration_ms = 5000.0,
        .emission_kind = "end",
        .termination_reason = "ota_reboot",
        .has_signal_level_dbfs = true,
        .signal_level_dbfs = -40.0,
        .measurements = &iv,
    };
    char *json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL);
    if (json) {
        EXPECT(strstr(json, "\"window_count\":0") != NULL);
        EXPECT(strstr(json, "\"rms_slope_db_per_s\":null") != NULL);
        free(json);
    }
}

int main(void)
{
    test_hierarchical_start();
    test_complete_requires_termination();
    test_end_empty_l0();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
