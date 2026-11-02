/**
 * Host unit tests for protocol-v1 Observation JSON builder (Phase 1).
 *
 * Build/run from repo root:
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     -I "$IDF_PATH/components/json/cJSON" \
 *     firmware/ear_components/ear_observation/observation_payload.c \
 *     "$IDF_PATH/components/json/cJSON/cJSON.c" \
 *     firmware/ear_components/ear_observation/test_host/test_observation_payload.c \
 *     -lm -o /tmp/test_dbmap_observation_payload && /tmp/test_dbmap_observation_payload
 */
#include "dbmap_observation_payload.h"

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

static void test_builds_null_classification_and_bearing(void)
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
    EXPECT(strstr(json, "source_family") == NULL);
    EXPECT(strstr(json, "\"signal_level_dbfs\":-42.5") != NULL);
    EXPECT(strstr(json, "\"duration_ms\":1000") != NULL);
    EXPECT(strstr(json, "\"timing_quality\":\"synchronized\"") != NULL);

    free(json);
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
    test_builds_null_classification_and_bearing();
    test_rejects_invalid_fields();

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
