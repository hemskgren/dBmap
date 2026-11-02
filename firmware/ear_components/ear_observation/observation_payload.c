#include "dbmap_observation_payload.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

char *dbmap_observation_build_json(const dbmap_observation_fields_t *fields)
{
    if (fields == NULL || fields->observation_id == NULL || fields->node_id == NULL ||
        fields->event_time_utc == NULL || fields->timing_quality == NULL) {
        return NULL;
    }
    if (fields->observation_id[0] == '\0' || fields->node_id[0] == '\0' ||
        fields->event_time_utc[0] == '\0' || fields->timing_quality[0] == '\0') {
        return NULL;
    }
    if (fields->capture_timestamp_monotonic_us < 0 || fields->timestamp_uncertainty_ms < 0.0 ||
        fields->duration_ms < 0.0 || fields->signal_level_dbfs > 0.0) {
        return NULL;
    }
    if (strcmp(fields->timing_quality, "synchronized") != 0 &&
        strcmp(fields->timing_quality, "estimated") != 0 &&
        strcmp(fields->timing_quality, "unsynchronized") != 0) {
        return NULL;
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
    /* Physical Ear Phase 1: activity/publisher only — no source family, no bearing. */
    cJSON_AddNullToObject(root, "classification");
    cJSON_AddNullToObject(root, "bearing");
    cJSON_AddNumberToObject(root, "signal_level_dbfs", fields->signal_level_dbfs);
    cJSON_AddNumberToObject(root, "duration_ms", fields->duration_ms);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}
