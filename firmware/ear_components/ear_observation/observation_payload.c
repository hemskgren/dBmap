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
        && (strcmp(k, "start") == 0 || strcmp(k, "periodic") == 0
            || strcmp(k, "end") == 0 || strcmp(k, "complete") == 0);
}

static bool termination_reason_ok(const char *r)
{
    return r != NULL
        && (strcmp(r, "natural") == 0 || strcmp(r, "invalid_hold") == 0
            || strcmp(r, "ota_reboot") == 0);
}

static bool kind_requires_termination(const char *k)
{
    return k != NULL && (strcmp(k, "end") == 0 || strcmp(k, "complete") == 0);
}

static cJSON *json_float_or_null(bool has, double value)
{
    return has ? cJSON_CreateNumber(value) : cJSON_CreateNull();
}

static cJSON *build_mean_stdev_object(bool has_mean, double mean, bool has_stdev, double stdev)
{
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    if (!cJSON_AddItemToObject(o, "mean", json_float_or_null(has_mean, mean))) {
        cJSON_Delete(o);
        return NULL;
    }
    if (!cJSON_AddItemToObject(o, "stdev", json_float_or_null(has_stdev, stdev))) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

static cJSON *build_signature_object(const dbmap_l1_interval_t *iv)
{
    if (iv == NULL) {
        return NULL;
    }
    cJSON *s = cJSON_CreateObject();
    if (s == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(s, "schema", "l1");
    cJSON_AddNumberToObject(s, "version", DBMAP_L1_MEASUREMENT_VERSION);
    cJSON_AddNumberToObject(s, "sample_rate_hz", (double)iv->sample_rate_hz);
    cJSON_AddNumberToObject(s, "analysis_window_ms", (double)iv->analysis_window_ms);
    cJSON_AddNumberToObject(s, "fft_size", (double)iv->fft_size);
    cJSON_AddNumberToObject(s, "freq_min_hz", (double)iv->freq_min_hz);
    cJSON_AddNumberToObject(s, "freq_max_hz", (double)iv->freq_max_hz);
    cJSON_AddNumberToObject(s, "window_count", (double)iv->window_count);
    cJSON_AddNumberToObject(s, "invalid_window_count", (double)iv->invalid_window_count);

    const bool has_stdev = iv->has_stdev;
    cJSON *centroid = build_mean_stdev_object(
        iv->has_centroid, iv->centroid_mean_hz, has_stdev && iv->has_centroid, iv->centroid_stdev_hz
    );
    cJSON *bandwidth = build_mean_stdev_object(
        iv->has_bandwidth, iv->bandwidth_mean_hz, has_stdev && iv->has_bandwidth, iv->bandwidth_stdev_hz
    );
    cJSON *dominant = build_mean_stdev_object(
        iv->has_dominant, iv->dominant_mean_hz, has_stdev && iv->has_dominant, iv->dominant_stdev_hz
    );
    cJSON *flatness = build_mean_stdev_object(
        iv->has_flatness, iv->flatness_mean, has_stdev && iv->has_flatness, iv->flatness_stdev
    );
    if (centroid == NULL || bandwidth == NULL || dominant == NULL || flatness == NULL) {
        cJSON_Delete(centroid);
        cJSON_Delete(bandwidth);
        cJSON_Delete(dominant);
        cJSON_Delete(flatness);
        cJSON_Delete(s);
        return NULL;
    }
    cJSON_AddItemToObject(s, "centroid_hz", centroid);
    cJSON_AddItemToObject(s, "bandwidth_hz", bandwidth);
    cJSON_AddItemToObject(s, "dominant_hz", dominant);
    cJSON_AddItemToObject(s, "flatness", flatness);
    return s;
}

static cJSON *build_l2_feature_object(
    uint32_t valid_count,
    bool has_mean,
    double mean,
    bool has_stdev,
    double stdev
)
{
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(o, "valid_count", (double)valid_count);
    if (!cJSON_AddItemToObject(o, "mean", json_float_or_null(has_mean, mean))) {
        cJSON_Delete(o);
        return NULL;
    }
    if (!cJSON_AddItemToObject(o, "stdev", json_float_or_null(has_stdev, stdev))) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

static cJSON *build_dynamics_object(const dbmap_l2_interval_t *iv)
{
    if (iv == NULL) {
        return NULL;
    }
    cJSON *d = cJSON_CreateObject();
    if (d == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(d, "schema", "l2");
    cJSON_AddNumberToObject(d, "version", DBMAP_L2_MEASUREMENT_VERSION);
    cJSON_AddNumberToObject(d, "sample_rate_hz", (double)iv->sample_rate_hz);
    cJSON_AddNumberToObject(d, "analysis_window_ms", (double)iv->analysis_window_ms);
    cJSON_AddNumberToObject(d, "fft_size", (double)iv->fft_size);
    cJSON_AddNumberToObject(d, "freq_min_hz", (double)iv->freq_min_hz);
    cJSON_AddNumberToObject(d, "freq_max_hz", (double)iv->freq_max_hz);
    cJSON_AddNumberToObject(d, "frame_hop_samples", (double)iv->frame_hop_samples);
    cJSON_AddNumberToObject(d, "window_count", (double)iv->window_count);
    cJSON_AddNumberToObject(d, "invalid_window_count", (double)iv->invalid_window_count);
    cJSON_AddBoolToObject(d, "history_overflow", iv->history_overflow);

    cJSON *flux = build_l2_feature_object(
        iv->spectral_flux_valid_count,
        iv->has_spectral_flux_mean,
        (double)iv->spectral_flux_mean,
        iv->has_spectral_flux_stdev,
        (double)iv->spectral_flux_stdev
    );
    cJSON *env = build_l2_feature_object(
        iv->envelope_cv_valid_count,
        iv->has_envelope_cv_mean,
        (double)iv->envelope_cv_mean,
        iv->has_envelope_cv_stdev,
        (double)iv->envelope_cv_stdev
    );
    cJSON *delta = build_l2_feature_object(
        iv->dominant_delta_valid_count,
        iv->has_dominant_delta_mean,
        (double)iv->dominant_delta_mean_hz,
        iv->has_dominant_delta_stdev,
        (double)iv->dominant_delta_stdev_hz
    );
    if (flux == NULL || env == NULL || delta == NULL) {
        cJSON_Delete(flux);
        cJSON_Delete(env);
        cJSON_Delete(delta);
        cJSON_Delete(d);
        return NULL;
    }
    cJSON_AddItemToObject(d, "spectral_flux", flux);
    cJSON_AddItemToObject(d, "envelope_cv", env);
    cJSON_AddItemToObject(d, "dominant_delta_hz", delta);
    return d;
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
        fields->event_time_utc == NULL || fields->timing_quality == NULL ||
        fields->emission_kind == NULL) {
        return NULL;
    }
    if (fields->observation_id[0] == '\0' || fields->node_id[0] == '\0' ||
        fields->event_time_utc[0] == '\0' || !timing_quality_ok(fields->timing_quality) ||
        !emission_kind_ok(fields->emission_kind)) {
        return NULL;
    }
    if (fields->capture_timestamp_monotonic_us < 0 || fields->timestamp_uncertainty_ms < 0.0 ||
        fields->interval_duration_ms < 0.0 || fields->episode_duration_ms < 0.0) {
        return NULL;
    }
    if (fields->has_signal_level_dbfs && fields->signal_level_dbfs > 0.0) {
        return NULL;
    }
    if (fields->measurements == NULL) {
        return NULL;
    }

    if (kind_requires_termination(fields->emission_kind)) {
        if (!termination_reason_ok(fields->termination_reason)) {
            return NULL;
        }
    } else if (fields->termination_reason != NULL) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "protocol_version", 1);
    cJSON_AddStringToObject(root, "message_type", "observation");

    cJSON *identity = cJSON_CreateObject();
    if (identity == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddStringToObject(identity, "observation_id", fields->observation_id);
    cJSON_AddStringToObject(identity, "node_id", fields->node_id);
    cJSON_AddStringToObject(
        identity, "hub_id", (fields->hub_id != NULL) ? fields->hub_id : ""
    );
    cJSON_AddNumberToObject(identity, "sequence_number", (double)fields->sequence_number);
    cJSON_AddItemToObject(root, "identity", identity);

    cJSON *context = cJSON_CreateObject();
    if (context == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddStringToObject(context, "event_time_utc", fields->event_time_utc);
    cJSON_AddNumberToObject(
        context,
        "capture_timestamp_monotonic_us",
        (double)fields->capture_timestamp_monotonic_us
    );
    cJSON_AddStringToObject(context, "timing_quality", fields->timing_quality);
    if (fields->has_clock_offset_ms) {
        cJSON_AddNumberToObject(context, "clock_offset_ms", fields->clock_offset_ms);
    } else {
        cJSON_AddNullToObject(context, "clock_offset_ms");
    }
    cJSON_AddNumberToObject(context, "timestamp_uncertainty_ms", fields->timestamp_uncertainty_ms);
    cJSON_AddNumberToObject(context, "interval_duration_ms", fields->interval_duration_ms);
    cJSON_AddNumberToObject(context, "episode_duration_ms", fields->episode_duration_ms);
    cJSON_AddStringToObject(context, "emission_kind", fields->emission_kind);
    if (fields->termination_reason != NULL) {
        cJSON_AddStringToObject(context, "termination_reason", fields->termination_reason);
    }
    cJSON_AddItemToObject(root, "context", context);

    cJSON *acoustic = cJSON_CreateObject();
    if (acoustic == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    if (fields->has_signal_level_dbfs) {
        cJSON_AddNumberToObject(acoustic, "signal_level_dbfs", fields->signal_level_dbfs);
    } else {
        cJSON_AddNullToObject(acoustic, "signal_level_dbfs");
    }
    cJSON *meas = build_measurements_object(fields->measurements);
    if (meas == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(acoustic, "measurements", meas);
    if (fields->signature == NULL) {
        cJSON_AddNullToObject(acoustic, "signature");
    } else {
        cJSON *sig = build_signature_object(fields->signature);
        if (sig == NULL) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToObject(acoustic, "signature", sig);
    }
    if (fields->dynamics == NULL) {
        cJSON_AddNullToObject(acoustic, "dynamics");
    } else {
        cJSON *dyn = build_dynamics_object(fields->dynamics);
        if (dyn == NULL) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToObject(acoustic, "dynamics", dyn);
    }
    cJSON_AddNullToObject(acoustic, "classification");
    cJSON_AddItemToObject(root, "acoustic", acoustic);

    cJSON *spatial = cJSON_CreateObject();
    if (spatial == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddNullToObject(spatial, "bearing");
    cJSON_AddNullToObject(spatial, "position");
    cJSON_AddItemToObject(root, "spatial", spatial);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}
