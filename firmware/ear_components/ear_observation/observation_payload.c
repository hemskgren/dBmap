#include "dbmap_observation_payload.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static bool timing_quality_ok(const char *q)
{
    return q != NULL
        && (strcmp(q, "synchronized") == 0 || strcmp(q, "estimated") == 0
            || strcmp(q, "unsynchronized") == 0);
}

static bool emission_kind_ok(const char *k)
{
    return k != NULL
        && (strcmp(k, "start") == 0 || strcmp(k, "periodic") == 0 || strcmp(k, "end") == 0);
}

static bool termination_reason_ok(const char *r)
{
    return r != NULL
        && (strcmp(r, "natural") == 0 || strcmp(r, "invalid_hold") == 0
            || strcmp(r, "ota_reboot") == 0);
}

static cJSON *json_float_or_null(bool has, double value)
{
    return has ? cJSON_CreateNumber(value) : cJSON_CreateNull();
}

static cJSON *build_measurements_object(const dbmap_l0_interval_t *iv)
{
    if (iv == NULL) {
        return NULL;
    }

    cJSON *m = cJSON_CreateObject();
    if (m == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(m, "schema", "l0");
    cJSON_AddNumberToObject(m, "version", DBMAP_L0_MEASUREMENT_VERSION);
    cJSON_AddNumberToObject(m, "band_plan_version", DBMAP_L0_BAND_PLAN_VERSION);
    cJSON_AddNumberToObject(m, "crest_threshold_version", DBMAP_L0_CREST_THRESHOLD_VERSION);
    cJSON_AddNumberToObject(m, "crest_threshold_db", (double)DBMAP_L0_CREST_THRESHOLD_DB);
    cJSON_AddNumberToObject(m, "mic_index", (double)iv->mic_index);
    cJSON_AddNumberToObject(m, "sample_rate_hz", (double)iv->sample_rate_hz);
    cJSON_AddNumberToObject(m, "analysis_window_ms", (double)iv->analysis_window_ms);
    cJSON_AddNumberToObject(m, "window_count", (double)iv->window_count);
    cJSON_AddNumberToObject(m, "clipped_window_count", (double)iv->clipped_window_count);
    cJSON_AddNumberToObject(m, "n_crest", (double)iv->n_crest);

    cJSON *rms = cJSON_CreateObject();
    if (rms == NULL) {
        cJSON_Delete(m);
        return NULL;
    }
    if (iv->has_rms_stats) {
        cJSON_AddNumberToObject(rms, "mean_dbfs", (double)iv->rms_mean_dbfs);
        cJSON_AddNumberToObject(rms, "min_dbfs", (double)iv->rms_min_dbfs);
        cJSON_AddNumberToObject(rms, "max_dbfs", (double)iv->rms_max_dbfs);
    } else {
        cJSON_AddNullToObject(rms, "mean_dbfs");
        cJSON_AddNullToObject(rms, "min_dbfs");
        cJSON_AddNullToObject(rms, "max_dbfs");
    }
    cJSON_AddItemToObject(m, "rms", rms);

    cJSON *bands = cJSON_CreateObject();
    cJSON *mean_arr = cJSON_CreateArray();
    cJSON *frac_arr = cJSON_CreateArray();
    if (bands == NULL || mean_arr == NULL || frac_arr == NULL) {
        cJSON_Delete(mean_arr);
        cJSON_Delete(frac_arr);
        cJSON_Delete(bands);
        cJSON_Delete(m);
        return NULL;
    }
    for (int b = 0; b < DBMAP_L0_BAND_COUNT; ++b) {
        if (iv->has_band_means) {
            cJSON_AddItemToArray(mean_arr, cJSON_CreateNumber((double)iv->band_mean_dbfs[b]));
        } else {
            cJSON_AddItemToArray(mean_arr, cJSON_CreateNull());
        }
        if (iv->has_band_fractions) {
            cJSON_AddItemToArray(frac_arr, cJSON_CreateNumber((double)iv->band_fraction[b]));
        } else {
            cJSON_AddItemToArray(frac_arr, cJSON_CreateNull());
        }
    }
    cJSON_AddItemToObject(bands, "mean_dbfs", mean_arr);
    cJSON_AddItemToObject(bands, "fraction", frac_arr);
    cJSON_AddItemToObject(m, "bands", bands);

    cJSON *hl = cJSON_CreateObject();
    if (hl == NULL) {
        cJSON_Delete(m);
        return NULL;
    }
    cJSON_AddItemToObject(hl, "mean_db", json_float_or_null(iv->has_hl_mean, (double)iv->hl_mean_db));
    cJSON_AddItemToObject(m, "hl", hl);

    cJSON *t1 = cJSON_CreateObject();
    if (t1 == NULL) {
        cJSON_Delete(m);
        return NULL;
    }
    if (iv->has_t1) {
        cJSON_AddNumberToObject(t1, "rms_stdev_db", (double)iv->rms_stdev_db);
        cJSON_AddNumberToObject(t1, "rms_range_db", (double)iv->rms_range_db);
    } else {
        cJSON_AddNullToObject(t1, "rms_stdev_db");
        cJSON_AddNullToObject(t1, "rms_range_db");
    }
    cJSON_AddItemToObject(m, "t1", t1);

    cJSON *t2 = cJSON_CreateObject();
    if (t2 == NULL) {
        cJSON_Delete(m);
        return NULL;
    }
    cJSON_AddItemToObject(
        t2, "rms_slope_db_per_s", json_float_or_null(iv->has_t2, (double)iv->rms_slope_db_per_s)
    );
    cJSON_AddItemToObject(m, "t2", t2);

    cJSON *t3 = cJSON_CreateObject();
    if (t3 == NULL) {
        cJSON_Delete(m);
        return NULL;
    }
    if (iv->has_t3) {
        cJSON_AddNumberToObject(t3, "crest_max_db", (double)iv->crest_max_db);
        cJSON_AddNumberToObject(t3, "high_crest_count", (double)iv->high_crest_count);
        cJSON_AddNumberToObject(t3, "high_crest_rate", (double)iv->high_crest_rate);
    } else {
        cJSON_AddNullToObject(t3, "crest_max_db");
        cJSON_AddNullToObject(t3, "high_crest_count");
        cJSON_AddNullToObject(t3, "high_crest_rate");
    }
    cJSON_AddItemToObject(m, "t3", t3);

    cJSON *t6 = cJSON_CreateObject();
    if (t6 == NULL) {
        cJSON_Delete(m);
        return NULL;
    }
    cJSON_AddItemToObject(
        t6, "hl_slope_db_per_s", json_float_or_null(iv->has_t6, (double)iv->hl_slope_db_per_s)
    );
    cJSON_AddItemToObject(m, "t6", t6);

    return m;
}

char *dbmap_observation_build_json(const dbmap_observation_fields_t *fields)
{
    if (fields == NULL || fields->observation_id == NULL || fields->node_id == NULL ||
        fields->event_time_utc == NULL || fields->timing_quality == NULL) {
        return NULL;
    }
    if (fields->observation_id[0] == '\0' || fields->node_id[0] == '\0' ||
        fields->event_time_utc[0] == '\0' || !timing_quality_ok(fields->timing_quality)) {
        return NULL;
    }
    if (fields->capture_timestamp_monotonic_us < 0 || fields->timestamp_uncertainty_ms < 0.0 ||
        fields->duration_ms < 0.0 || fields->signal_level_dbfs > 0.0) {
        return NULL;
    }

    const bool want_ext = (fields->emission_kind != NULL) || (fields->measurements != NULL);
    if (want_ext) {
        if (!emission_kind_ok(fields->emission_kind) || fields->measurements == NULL) {
            return NULL;
        }
    }

    if (fields->termination_reason != NULL) {
        if (!want_ext || strcmp(fields->emission_kind, "end") != 0
            || !termination_reason_ok(fields->termination_reason)) {
            return NULL;
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "protocol_version", 1);
    cJSON_AddStringToObject(root, "message_type", "observation");
    cJSON_AddStringToObject(root, "observation_id", fields->observation_id);
    cJSON_AddStringToObject(root, "node_id", fields->node_id);
    cJSON_AddNumberToObject(root, "sequence_number", (double)fields->sequence_number);
    cJSON_AddStringToObject(root, "event_time_utc", fields->event_time_utc);
    cJSON_AddNumberToObject(
        root,
        "capture_timestamp_monotonic_us",
        (double)fields->capture_timestamp_monotonic_us
    );
    cJSON_AddStringToObject(root, "timing_quality", fields->timing_quality);
    cJSON_AddNullToObject(root, "clock_offset_ms");
    cJSON_AddNumberToObject(root, "timestamp_uncertainty_ms", fields->timestamp_uncertainty_ms);
    cJSON_AddNullToObject(root, "classification");
    cJSON_AddNullToObject(root, "bearing");
    cJSON_AddNumberToObject(root, "signal_level_dbfs", fields->signal_level_dbfs);
    cJSON_AddNumberToObject(root, "duration_ms", fields->duration_ms);

    if (want_ext) {
        cJSON_AddStringToObject(root, "emission_kind", fields->emission_kind);
        if (fields->termination_reason != NULL) {
            cJSON_AddStringToObject(root, "termination_reason", fields->termination_reason);
        }
        cJSON *meas = build_measurements_object(fields->measurements);
        if (meas == NULL) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToObject(root, "measurements", meas);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}
