/**
 * Host unit tests for Observation JSON builder (protocol v1 + optional L0).
 *
 * Build/run from repo root:
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     -I firmware/ear_components/ear_mic/include \
 *     -I "$IDF_PATH/components/json/cJSON" \
 *     firmware/ear_components/ear_observation/observation_payload.c \
 *     "$IDF_PATH/components/json/cJSON/cJSON.c" \
 *     firmware/ear_components/ear_observation/test_host/test_observation_payload.c \
 *     -lm -o /tmp/test_dbmap_observation_payload && /tmp/test_dbmap_observation_payload
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

static dbmap_l0_interval_t sample_interval(void)
{
    dbmap_l0_interval_t iv;
    memset(&iv, 0, sizeof(iv));
    iv.window_count = 5;
    iv.clipped_window_count = 1;
    iv.n_crest = 4;
    iv.analysis_window_ms = 1000;
    iv.sample_rate_hz = 16000;
    iv.mic_index = 0;
    iv.has_rms_stats = true;
    iv.rms_mean_dbfs = -42.5f;
    iv.rms_min_dbfs = -50.0f;
    iv.rms_max_dbfs = -35.0f;
    /* Cross-field consistent with mean linear E = {4e-5, 3e-5, 2e-5, 1e-5}:
     * mean_dbfs = 10*log10(E_k), fraction = E_k/ΣE, hl = 10*log10((E2+E3)/(E0+E1)). */
    iv.has_band_means = true;
    iv.band_mean_dbfs[0] = -43.9794f;
    iv.band_mean_dbfs[1] = -45.228787f;
    iv.band_mean_dbfs[2] = -46.9897f;
    iv.band_mean_dbfs[3] = -50.0f;
    iv.has_band_fractions = true;
    iv.band_fraction[0] = 0.4f;
    iv.band_fraction[1] = 0.3f;
    iv.band_fraction[2] = 0.2f;
    iv.band_fraction[3] = 0.1f;
    iv.has_hl_mean = true;
    iv.hl_mean_db = -3.679768f;
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
    iv.interval_start_mono_us = 1000;
    iv.interval_end_mono_us = 6000;
    return iv;
}

static void test_legacy_v1_omits_extension(void)
{
    dbmap_observation_fields_t fields = {
        .observation_id = "11111111-2222-4333-8444-555555555555",
        .node_id = "EAR-001",
        .sequence_number = 7,
        .event_time_utc = "2026-11-02T00:00:00Z",
        .capture_timestamp_monotonic_us = 123456789,
        .timing_quality = "synchronized",
        .timestamp_uncertainty_ms = 100.0,
        .signal_level_dbfs = -42.5,
        .duration_ms = 1000.0,
        .emission_kind = NULL,
        .termination_reason = NULL,
        .measurements = NULL,
    };

    char *json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL);
    if (json == NULL) {
        return;
    }

    EXPECT(strstr(json, "\"protocol_version\":1") != NULL);
    EXPECT(strstr(json, "\"message_type\":\"observation\"") != NULL);
    EXPECT(strstr(json, "\"observation_id\":\"11111111-2222-4333-8444-555555555555\"") != NULL);
    EXPECT(strstr(json, "\"node_id\":\"EAR-001\"") != NULL);
    EXPECT(strstr(json, "\"sequence_number\":7") != NULL);
    EXPECT(strstr(json, "\"classification\":null") != NULL);
    EXPECT(strstr(json, "\"bearing\":null") != NULL);
    EXPECT(strstr(json, "\"clock_offset_ms\":null") != NULL);
    EXPECT(strstr(json, "simulated_source_id") == NULL);
    EXPECT(strstr(json, "\"signal_level_dbfs\":-42.5") != NULL);
    EXPECT(strstr(json, "\"duration_ms\":1000") != NULL);
    EXPECT(strstr(json, "emission_kind") == NULL);
    EXPECT(strstr(json, "measurements") == NULL);

    free(json);
}

static void test_extension_requires_kind_and_measurements(void)
{
    dbmap_l0_interval_t iv = sample_interval();
    dbmap_observation_fields_t fields = {
        .observation_id = "11111111-2222-4333-8444-555555555555",
        .node_id = "EAR-001",
        .sequence_number = 1,
        .event_time_utc = "2026-11-02T00:00:00Z",
        .capture_timestamp_monotonic_us = 1000,
        .timing_quality = "synchronized",
        .timestamp_uncertainty_ms = 10.0,
        .signal_level_dbfs = -40.0,
        .duration_ms = 5100.0,
        .emission_kind = NULL,
        .termination_reason = NULL,
        .measurements = &iv,
    };
    EXPECT(dbmap_observation_build_json(&fields) == NULL);

    fields.emission_kind = "periodic";
    fields.measurements = NULL;
    EXPECT(dbmap_observation_build_json(&fields) == NULL);

    fields.emission_kind = "nope";
    fields.measurements = &iv;
    EXPECT(dbmap_observation_build_json(&fields) == NULL);
}

static void test_extended_v1_serializes_measurements_and_kind(void)
{
    dbmap_l0_interval_t iv = sample_interval();
    dbmap_observation_fields_t fields = {
        .observation_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
        .node_id = "EAR-001",
        .sequence_number = 42,
        .event_time_utc = "2026-11-03T12:00:00Z",
        .capture_timestamp_monotonic_us = 15100000,
        .timing_quality = "estimated",
        .timestamp_uncertainty_ms = 25.0,
        .signal_level_dbfs = -46.0,
        .duration_ms = 5100.0,
        .emission_kind = "periodic",
        .termination_reason = NULL,
        .measurements = &iv,
    };

    char *json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL);
    if (json == NULL) {
        return;
    }

    EXPECT(strstr(json, "\"protocol_version\":1") != NULL);
    EXPECT(strstr(json, "\"protocol_version\":2") == NULL);
    EXPECT(strstr(json, "\"emission_kind\":\"periodic\"") != NULL);
    EXPECT(strstr(json, "termination_reason") == NULL);
    EXPECT(strstr(json, "\"signal_level_dbfs\":-46") != NULL);
    EXPECT(strstr(json, "\"measurements\":{") != NULL);
    EXPECT(strstr(json, "\"schema\":\"l0\"") != NULL);
    EXPECT(strstr(json, "\"version\":1") != NULL);
    EXPECT(strstr(json, "\"band_plan_version\":1") != NULL);
    EXPECT(strstr(json, "\"crest_threshold_version\":1") != NULL);
    EXPECT(strstr(json, "\"crest_threshold_db\":12") != NULL);
    EXPECT(strstr(json, "\"window_count\":5") != NULL);
    EXPECT(strstr(json, "\"clipped_window_count\":1") != NULL);
    EXPECT(strstr(json, "\"n_crest\":4") != NULL);
    EXPECT(strstr(json, "\"mean_dbfs\":-42.5") != NULL);
    EXPECT(strstr(json, "\"hl\":{") != NULL);
    EXPECT(strstr(json, "\"mean_db\":-3.679768") != NULL);
    EXPECT(strstr(json, "\"fraction\":[0.4") != NULL);
    EXPECT(strstr(json, "\"t1\":{") != NULL);
    EXPECT(strstr(json, "\"t2\":{") != NULL);
    EXPECT(strstr(json, "\"t3\":{") != NULL);
    EXPECT(strstr(json, "\"t6\":{") != NULL);
    EXPECT(strstr(json, "\"classification\":null") != NULL);
    EXPECT(strstr(json, "\"bearing\":null") != NULL);

    /* Write representative sample for Checkpoint C review (stderr). */
    fprintf(stderr, "REPRESENTATIVE_EXTENDED_OBSERVATION_JSON\n%s\n", json);

    free(json);
    fields.emission_kind = "start";
    json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL && strstr(json, "\"emission_kind\":\"start\"") != NULL);
    free(json);
    fields.emission_kind = "end";
    json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL && strstr(json, "\"emission_kind\":\"end\"") != NULL);
    free(json);
}

static void test_extended_null_optional_measurement_fields(void)
{
    dbmap_l0_interval_t iv;
    memset(&iv, 0, sizeof(iv));
    iv.analysis_window_ms = 1000;
    iv.sample_rate_hz = 16000;
    /* window_count 0 → no has_* flags */
    dbmap_observation_fields_t fields = {
        .observation_id = "11111111-2222-4333-8444-555555555555",
        .node_id = "EAR-001",
        .sequence_number = 0,
        .event_time_utc = "2026-11-02T00:00:00Z",
        .capture_timestamp_monotonic_us = 1,
        .timing_quality = "synchronized",
        .timestamp_uncertainty_ms = 10.0,
        .signal_level_dbfs = -60.0,
        .duration_ms = 2000.0,
        .emission_kind = "start",
        .termination_reason = NULL,
        .measurements = &iv,
    };
    char *json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL);
    if (json) {
        EXPECT(strstr(json, "\"protocol_version\":1") != NULL);
        EXPECT(strstr(json, "\"window_count\":0") != NULL);
        EXPECT(strstr(json, "\"mean_db\":null") != NULL);
        EXPECT(strstr(json, "\"rms_slope_db_per_s\":null") != NULL);
        free(json);
    }
}

/** NEW: termination_reason on END only. */
static void test_termination_reason_end_only(void)
{
    dbmap_l0_interval_t iv = sample_interval();
    dbmap_observation_fields_t fields = {
        .observation_id = "11111111-2222-4333-8444-555555555555",
        .node_id = "EAR-001",
        .sequence_number = 3,
        .event_time_utc = "2026-11-02T00:00:00Z",
        .capture_timestamp_monotonic_us = 1000,
        .timing_quality = "synchronized",
        .timestamp_uncertainty_ms = 10.0,
        .signal_level_dbfs = -40.0,
        .duration_ms = 1000.0,
        .emission_kind = "end",
        .termination_reason = NULL,
        .measurements = &iv,
    };

    /* Legacy END: field omitted. */
    char *json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL);
    EXPECT(json && strstr(json, "termination_reason") == NULL);
    free(json);

    fields.termination_reason = "natural";
    json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL && strstr(json, "\"termination_reason\":\"natural\"") != NULL);
    free(json);

    fields.termination_reason = "invalid_hold";
    json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL && strstr(json, "\"termination_reason\":\"invalid_hold\"") != NULL);
    free(json);

    fields.termination_reason = "ota_reboot";
    json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL && strstr(json, "\"termination_reason\":\"ota_reboot\"") != NULL);
    free(json);

    fields.termination_reason = "unknown";
    EXPECT(dbmap_observation_build_json(&fields) == NULL);

    fields.emission_kind = "periodic";
    fields.termination_reason = "natural";
    EXPECT(dbmap_observation_build_json(&fields) == NULL);

    fields.emission_kind = "start";
    EXPECT(dbmap_observation_build_json(&fields) == NULL);
}

static void test_rejects_invalid_fields(void)
{
    dbmap_observation_fields_t fields = {
        .observation_id = "11111111-2222-4333-8444-555555555555",
        .node_id = "EAR-001",
        .sequence_number = 0,
        .event_time_utc = "2026-11-02T00:00:00Z",
        .capture_timestamp_monotonic_us = 1,
        .timing_quality = "synchronized",
        .timestamp_uncertainty_ms = 10.0,
        .signal_level_dbfs = -10.0,
        .duration_ms = 1000.0,
    };

    fields.signal_level_dbfs = 0.1;
    EXPECT(dbmap_observation_build_json(&fields) == NULL);
    fields.signal_level_dbfs = -10.0;

    fields.duration_ms = -1.0;
    EXPECT(dbmap_observation_build_json(&fields) == NULL);
    fields.duration_ms = 1000.0;

    fields.timing_quality = "pretty_sure";
    EXPECT(dbmap_observation_build_json(&fields) == NULL);
    fields.timing_quality = "synchronized";

    fields.node_id = "";
    EXPECT(dbmap_observation_build_json(&fields) == NULL);

    EXPECT(dbmap_observation_build_json(NULL) == NULL);
}

int main(void)
{
    /* Existing */
    test_legacy_v1_omits_extension();
    test_extension_requires_kind_and_measurements();
    test_extended_v1_serializes_measurements_and_kind();
    test_extended_null_optional_measurement_fields();
    test_rejects_invalid_fields();
    /* Newly added */
    test_termination_reason_end_only();

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
