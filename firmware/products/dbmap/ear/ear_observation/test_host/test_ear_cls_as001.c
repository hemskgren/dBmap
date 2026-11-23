/**
 * Host tests: AS-001 assessment lifecycle + Observation JSON serialization.
 *
 * START → AIRCRAFT only; PERIODIC → AIRCRAFT > Helicopter.
 * Gate: firmware/scripts/run_firmware_fast_gate.sh (ear_cls_as001).
 */
#include "dbmap_observation_payload.h"
#include "ear_cls_as001.h"

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
    iv.sample_rate_hz = 16000;
    iv.analysis_window_ms = 1000;
    iv.window_count = 1;
    iv.has_rms_stats = true;
    iv.rms_mean_dbfs = -40.0f;
    iv.rms_min_dbfs = -42.0f;
    iv.rms_max_dbfs = -38.0f;
    return iv;
}

static char *build_json(const ear_cls_assessment_wire_t *assess)
{
    dbmap_l0_interval_t iv = sample_interval();
    dbmap_observation_fields_t fields;
    memset(&fields, 0, sizeof(fields));
    fields.observation_id = "00000000-0000-4000-8000-000000000001";
    fields.node_id = "TST-EAR-001";
    fields.hub_id = "hub-test";
    fields.sequence_number = 1u;
    fields.event_time_utc = "2026-01-01T00:00:00Z";
    fields.capture_timestamp_monotonic_us = 1000000;
    fields.timing_quality = "unsynchronized";
    fields.timestamp_uncertainty_ms = 60000.0;
    fields.interval_duration_ms = 5000.0;
    fields.episode_duration_ms = 5000.0;
    fields.emission_kind = "start";
    fields.measurements = &iv;
    fields.assessment = assess;
    return dbmap_observation_build_json(&fields);
}

static void test_start_parent_only(void)
{
    ear_cls_assessment_wire_t w;
    EXPECT(ear_cls_as001_apply(&w, 2.0f, EAR_CLS_PHASE_START));
    EXPECT(w.present);
    EXPECT(w.n_nodes == 1u);
    EXPECT(strcmp(w.nodes[0].id, "AIRCRAFT") == 0);
    EXPECT(strcmp(w.nodes[1].id, "") == 0 || w.n_nodes < 2u);

    char *json = build_json(&w);
    EXPECT(json != NULL);
    if (json != NULL) {
        EXPECT(strstr(json, "\"assessment\":null") == NULL);
        EXPECT(strstr(json, "\"id\":\"AIRCRAFT\"") != NULL);
        /* Regression: START must not already include leaf. */
        EXPECT(strstr(json, "\"id\":\"Helicopter\"") == NULL);
        EXPECT(strstr(json, "\"id\":\"ALARM\"") == NULL);
        fprintf(stderr, "AS001_START_JSON_SNIPPET %s\n", json);
        free(json);
    }
}

static void test_periodic_deepens_leaf(void)
{
    ear_cls_assessment_wire_t w;
    EXPECT(ear_cls_as001_apply(&w, 2.0f, EAR_CLS_PHASE_START));
    EXPECT(w.n_nodes == 1u);

    EXPECT(ear_cls_as001_apply(&w, 2.0f, EAR_CLS_PHASE_PERIODIC));
    EXPECT(w.present);
    EXPECT(w.n_nodes == 2u);
    EXPECT(strcmp(w.nodes[0].id, "AIRCRAFT") == 0);
    EXPECT(strcmp(w.nodes[1].id, "Helicopter") == 0);
    /* Independent per-node confidence: root at least as high as leaf. */
    EXPECT(w.nodes[0].confidence >= w.nodes[1].confidence - 1e-6f);
    EXPECT(fabsf(w.nodes[0].confidence - w.nodes[1].confidence) > 1e-4f);

    char *json = build_json(&w);
    EXPECT(json != NULL);
    if (json != NULL) {
        EXPECT(strstr(json, "\"id\":\"AIRCRAFT\"") != NULL);
        EXPECT(strstr(json, "\"id\":\"Helicopter\"") != NULL);
        fprintf(stderr, "AS001_PERIODIC_JSON_SNIPPET %s\n", json);
        free(json);
    }

    /* Idempotent second PERIODIC. */
    EXPECT(ear_cls_as001_apply(&w, 2.0f, EAR_CLS_PHASE_PERIODIC));
    EXPECT(w.n_nodes == 2u);
}

static void test_end_deepens_if_needed(void)
{
    ear_cls_assessment_wire_t w;
    EXPECT(ear_cls_as001_apply(&w, 1.0f, EAR_CLS_PHASE_START));
    EXPECT(w.n_nodes == 1u);
    EXPECT(ear_cls_as001_apply(&w, 1.0f, EAR_CLS_PHASE_END));
    EXPECT(w.n_nodes == 2u);
    EXPECT(strcmp(w.nodes[1].id, "Helicopter") == 0);
}

static void test_fill_assessment_is_start(void)
{
    ear_cls_assessment_wire_t w;
    EXPECT(ear_cls_as001_fill_assessment(&w, 1.5f));
    EXPECT(w.n_nodes == 1u);
    EXPECT(strcmp(w.nodes[0].id, "AIRCRAFT") == 0);
}

static void test_null_out_rejected(void)
{
    EXPECT(!ear_cls_as001_apply(NULL, 1.0f, EAR_CLS_PHASE_START));
    EXPECT(!ear_cls_as001_fill_assessment(NULL, 1.0f));
}

int main(void)
{
    test_start_parent_only();
    test_periodic_deepens_leaf();
    test_end_deepens_if_needed();
    test_fill_assessment_is_start();
    test_null_out_rejected();
    if (g_failures) {
        fprintf(stderr, "test_ear_cls_as001: %d failure(s)\n", g_failures);
        return 1;
    }
    puts("test_ear_cls_as001: ok");
    return 0;
}
