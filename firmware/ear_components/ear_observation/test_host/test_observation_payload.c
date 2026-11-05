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

static dbmap_observation_fields_t base_fields(const dbmap_l0_interval_t *iv)
{
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
        .measurements = iv,
        .signature = NULL,
        .dynamics = NULL,
    };
    return fields;
}

static dbmap_l2_interval_t sample_dynamics(void)
{
    dbmap_l2_interval_t d;
    memset(&d, 0, sizeof(d));
    d.sample_rate_hz = 16000;
    d.analysis_window_ms = 1000;
    d.fft_size = 1024;
    d.freq_min_hz = 80.0f;
    d.freq_max_hz = 7600.0f;
    d.frame_hop_samples = 1024;
    d.window_count = 5;
    d.invalid_window_count = 0;
    d.history_overflow = false;

    /* Prefer binary-exact floats so cJSON print matches substring checks. */
    d.spectral_flux_valid_count = 5;
    d.has_spectral_flux_mean = true;
    d.spectral_flux_mean = 0.5f;
    d.has_spectral_flux_stdev = true;
    d.spectral_flux_stdev = 0.25f;

    d.envelope_cv_valid_count = 5;
    d.has_envelope_cv_mean = true;
    d.envelope_cv_mean = 0.125f;
    d.has_envelope_cv_stdev = true;
    d.envelope_cv_stdev = 0.0625f;

    d.dominant_delta_valid_count = 5;
    d.has_dominant_delta_mean = true;
    d.dominant_delta_mean_hz = 15.625f; /* 1000/64; exact in float32 */
    d.has_dominant_delta_stdev = true;
    d.dominant_delta_stdev_hz = 0.0f;
    return d;
}

static void test_hierarchical_start(void)
{
    dbmap_l0_interval_t iv = sample_interval();
    dbmap_observation_fields_t fields = base_fields(&iv);
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
        EXPECT(strstr(json, "\"dynamics\":null") != NULL);
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
        .signature = NULL,
        .dynamics = NULL,
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
        EXPECT(strstr(json, "\"dynamics\":null") != NULL);
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
        .signature = NULL,
        .dynamics = NULL,
    };
    char *json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL);
    if (json) {
        EXPECT(strstr(json, "\"window_count\":0") != NULL);
        EXPECT(strstr(json, "\"rms_slope_db_per_s\":null") != NULL);
        EXPECT(strstr(json, "\"dynamics\":null") != NULL);
        free(json);
    }
}

static void test_dynamics_valid_object(void)
{
    dbmap_l0_interval_t iv = sample_interval();
    dbmap_l2_interval_t dyn = sample_dynamics();
    dbmap_observation_fields_t fields = base_fields(&iv);
    fields.dynamics = &dyn;

    char *json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL);
    if (json) {
        EXPECT(strstr(json, "\"dynamics\":null") == NULL);
        EXPECT(strstr(json, "\"schema\":\"l2\"") != NULL);
        EXPECT(strstr(json, "\"version\":1") != NULL);
        EXPECT(strstr(json, "\"sample_rate_hz\":16000") != NULL);
        EXPECT(strstr(json, "\"analysis_window_ms\":1000") != NULL);
        EXPECT(strstr(json, "\"fft_size\":1024") != NULL);
        EXPECT(strstr(json, "\"freq_min_hz\":80") != NULL);
        EXPECT(strstr(json, "\"freq_max_hz\":7600") != NULL);
        EXPECT(strstr(json, "\"frame_hop_samples\":1024") != NULL);
        EXPECT(strstr(json, "\"history_overflow\":false") != NULL);
        EXPECT(strstr(json, "\"spectral_flux\"") != NULL);
        EXPECT(strstr(json, "\"envelope_cv\"") != NULL);
        EXPECT(strstr(json, "\"dominant_delta_hz\"") != NULL);
        EXPECT(strstr(json, "\"valid_count\":5") != NULL);
        EXPECT(strstr(json, "\"mean\":0.5") != NULL);
        EXPECT(strstr(json, "\"stdev\":0.25") != NULL);
        EXPECT(strstr(json, "\"mean\":0.125") != NULL);
        EXPECT(strstr(json, "\"stdev\":0.0625") != NULL);
        EXPECT(strstr(json, "15.625") != NULL);
        free(json);
    }
}

static void test_dynamics_null(void)
{
    dbmap_l0_interval_t iv = sample_interval();
    dbmap_observation_fields_t fields = base_fields(&iv);
    fields.dynamics = NULL;
    char *json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL);
    if (json) {
        EXPECT(strstr(json, "\"dynamics\":null") != NULL);
        EXPECT(strstr(json, "\"schema\":\"l2\"") == NULL);
        free(json);
    }
}

static void test_dynamics_feature_null_rules(void)
{
    dbmap_l0_interval_t iv = sample_interval();
    dbmap_l2_interval_t dyn;
    memset(&dyn, 0, sizeof(dyn));
    dyn.sample_rate_hz = 16000;
    dyn.analysis_window_ms = 1000;
    dyn.fft_size = 1024;
    dyn.freq_min_hz = 80.0f;
    dyn.freq_max_hz = 7600.0f;
    dyn.frame_hop_samples = 1024;
    dyn.window_count = 1;
    dyn.invalid_window_count = 0;
    dyn.history_overflow = false;

    /* valid_count=1 → mean present, stdev null */
    dyn.spectral_flux_valid_count = 1;
    dyn.has_spectral_flux_mean = true;
    dyn.spectral_flux_mean = 0.5f;
    dyn.has_spectral_flux_stdev = false;

    dyn.envelope_cv_valid_count = 1;
    dyn.has_envelope_cv_mean = true;
    dyn.envelope_cv_mean = 0.25f;
    dyn.has_envelope_cv_stdev = false;

    /* valid_count=0 → mean and stdev null */
    dyn.dominant_delta_valid_count = 0;
    dyn.has_dominant_delta_mean = false;
    dyn.has_dominant_delta_stdev = false;

    dbmap_observation_fields_t fields = base_fields(&iv);
    fields.dynamics = &dyn;
    char *json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL);
    if (json) {
        EXPECT(strstr(json, "\"window_count\":1") != NULL);
        EXPECT(strstr(json, "\"spectral_flux\":{\"valid_count\":1,\"mean\":0.5,\"stdev\":null}") != NULL);
        EXPECT(strstr(json, "\"envelope_cv\":{\"valid_count\":1,\"mean\":0.25,\"stdev\":null}") != NULL);
        EXPECT(
            strstr(
                json,
                "\"dominant_delta_hz\":{\"valid_count\":0,\"mean\":null,\"stdev\":null}"
            ) != NULL
        );
        free(json);
    }
}

static void test_dynamics_overflow_option_a(void)
{
    dbmap_l0_interval_t iv = sample_interval();
    dbmap_l2_interval_t dyn;
    memset(&dyn, 0, sizeof(dyn));
    dyn.sample_rate_hz = 16000;
    dyn.analysis_window_ms = 1000;
    dyn.fft_size = 1024;
    dyn.freq_min_hz = 80.0f;
    dyn.freq_max_hz = 7600.0f;
    dyn.frame_hop_samples = 1024;
    dyn.window_count = 0;
    dyn.invalid_window_count = 0;
    dyn.history_overflow = true;
    /* Option A: feature counts cleared, means/stdevs absent */
    dyn.spectral_flux_valid_count = 0;
    dyn.envelope_cv_valid_count = 0;
    dyn.dominant_delta_valid_count = 0;

    dbmap_observation_fields_t fields = base_fields(&iv);
    fields.dynamics = &dyn;
    char *json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL);
    if (json) {
        EXPECT(strstr(json, "\"schema\":\"l2\"") != NULL);
        EXPECT(strstr(json, "\"version\":1") != NULL);
        EXPECT(strstr(json, "\"history_overflow\":true") != NULL);
        EXPECT(strstr(json, "\"window_count\":0") != NULL);
        EXPECT(strstr(json, "\"invalid_window_count\":0") != NULL);
        EXPECT(
            strstr(
                json, "\"spectral_flux\":{\"valid_count\":0,\"mean\":null,\"stdev\":null}"
            ) != NULL
        );
        EXPECT(
            strstr(
                json, "\"envelope_cv\":{\"valid_count\":0,\"mean\":null,\"stdev\":null}"
            ) != NULL
        );
        EXPECT(
            strstr(
                json,
                "\"dominant_delta_hz\":{\"valid_count\":0,\"mean\":null,\"stdev\":null}"
            ) != NULL
        );
        free(json);
    }
}

static void test_dynamics_schema_and_version(void)
{
    dbmap_l0_interval_t iv = empty_interval();
    dbmap_l2_interval_t dyn;
    memset(&dyn, 0, sizeof(dyn));
    dyn.sample_rate_hz = 16000;
    dyn.analysis_window_ms = 1000;
    dyn.fft_size = DBMAP_L1_FFT_SIZE;
    dyn.freq_min_hz = DBMAP_L1_FREQ_MIN_HZ;
    dyn.freq_max_hz = DBMAP_L1_FREQ_MAX_HZ;
    dyn.frame_hop_samples = DBMAP_L1_FFT_SIZE;
    dyn.history_overflow = false;

    dbmap_observation_fields_t fields = base_fields(&iv);
    fields.emission_kind = "periodic";
    fields.dynamics = &dyn;
    char *json = dbmap_observation_build_json(&fields);
    EXPECT(json != NULL);
    if (json) {
        EXPECT(strstr(json, "\"schema\":\"l2\"") != NULL);
        EXPECT(DBMAP_L2_MEASUREMENT_VERSION == 1);
        EXPECT(strstr(json, "\"version\":1") != NULL);
        EXPECT(strstr(json, "\"fft_size\":1024") != NULL);
        EXPECT(strstr(json, "\"frame_hop_samples\":1024") != NULL);
        EXPECT(strstr(json, "\"history_overflow\":false") != NULL);
        free(json);
    }
}

int main(void)
{
    test_hierarchical_start();
    test_complete_requires_termination();
    test_end_empty_l0();
    test_dynamics_valid_object();
    test_dynamics_null();
    test_dynamics_feature_null_rules();
    test_dynamics_overflow_option_a();
    test_dynamics_schema_and_version();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
