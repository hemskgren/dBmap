#include "dbmap_ear_observation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "dbmap_activity_detector.h"
#include "dbmap_ear_mic.h"
#include "dbmap_health.h"
#include "dbmap_mqtt.h"
#include "dbmap_observation_seq.h"
#include "dbmap_time.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "ear_obs";

static const char *NVS_NS = "ear_obs";
static const char *NVS_KEY_SEQ_LIMIT = "seq_lim";
static const char *NVS_KEY_SEQ_LEGACY = "seq"; /* Phase 1 next-value; migrated as limit */

static const dbmap_effective_t *s_cfg;
static dbmap_obs_seq_t s_seq;
static bool s_started;
static bool s_test_published;
static bool s_logged_wait_mqtt;

#if CONFIG_DBMAP_EAR_OBS_RMS_ACTIVITY
static bool s_logged_mqtt_drop;
static uint32_t s_dropped_while_mqtt_down;
static dbmap_activity_detector_t s_detector;
#endif

/*
 * Sequence persistence (Phase 2)
 * ------------------------------
 * NVS key seq_lim holds an exclusive upper bound of a reserved block.
 * RAM hands out numbers in [next, limit). On boot, next starts at limit so any
 * unused remainder of the previous block is skipped (gaps OK).
 *
 * Consumption: when dbmap_obs_seq_allocate() returns N, N is consumed. The block
 * containing N was already persisted. A failed MQTT publish does not reclaim N.
 *
 * MQTT down: do not allocate; drop the emission; detection continues.
 */

static esp_err_t seq_limit_load(uint32_t *out_limit, uint32_t block_size)
{
    if (out_limit == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_limit = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    uint32_t limit = 0;
    err = nvs_get_u32(h, NVS_KEY_SEQ_LIMIT, &limit);
    if (err == ESP_OK) {
        nvs_close(h);
        *out_limit = limit;
        return ESP_OK;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return err;
    }

    /*
     * Phase 1 key `seq` = next sequence to assign (written to NVS *before*
     * MQTT publish after allocating). Numbers < legacy are consumed.
     * Skip forward one block so we never reuse a number that may have been
     * published in a crash window.
     */
    uint32_t legacy_next = 0;
    err = nvs_get_u32(h, NVS_KEY_SEQ_LEGACY, &legacy_next);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out_limit = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    *out_limit = dbmap_obs_seq_migrate_legacy_next(legacy_next, block_size);
    ESP_LOGW(
        TAG,
        "migrated Phase 1 seq=%lu → seq_lim=%lu (skip-forward block=%lu)",
        (unsigned long)legacy_next,
        (unsigned long)*out_limit,
        (unsigned long)(block_size == 0 ? 1 : block_size)
    );
    return ESP_OK;
}

static esp_err_t seq_limit_store(uint32_t limit)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(h, NVS_KEY_SEQ_LIMIT, limit);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t seq_ensure_reserved(void)
{
    if (!dbmap_obs_seq_needs_reserve(&s_seq)) {
        return ESP_OK;
    }
    uint32_t new_limit = dbmap_obs_seq_reserve(&s_seq);
    esp_err_t err = seq_limit_store(new_limit);
    if (err != ESP_OK) {
        /* Roll back RAM limit so we do not hand out unpersisted numbers. */
        s_seq.limit = s_seq.next;
        return err;
    }
    ESP_LOGI(
        TAG,
        "reserved sequence block next=%lu limit=%lu",
        (unsigned long)s_seq.next,
        (unsigned long)s_seq.limit
    );
    return ESP_OK;
}

static esp_err_t seq_allocate_consumed(uint32_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = seq_ensure_reserved();
    if (err != ESP_OK) {
        return err;
    }
    if (!dbmap_obs_seq_allocate(&s_seq, out)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void format_uuid_v4(char out[37])
{
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = (uint8_t)((bytes[6] & 0x0f) | 0x40);
    bytes[8] = (uint8_t)((bytes[8] & 0x3f) | 0x80);
    snprintf(
        out,
        37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0],
        bytes[1],
        bytes[2],
        bytes[3],
        bytes[4],
        bytes[5],
        bytes[6],
        bytes[7],
        bytes[8],
        bytes[9],
        bytes[10],
        bytes[11],
        bytes[12],
        bytes[13],
        bytes[14],
        bytes[15]
    );
}

static esp_err_t format_event_time_for_interval_start(
    int64_t interval_start_mono_us,
    char event_time[40],
    const char **timing_quality,
    double *uncertainty_ms
)
{
    if (event_time == NULL || timing_quality == NULL || uncertainty_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t now_epoch_us = 0;
    int64_t now_mono = (int64_t)esp_timer_get_time();
    esp_err_t time_err = dbmap_time_get_epoch_us(&now_epoch_us);
    if (time_err == ESP_OK) {
        int64_t start_epoch_us = now_epoch_us - (now_mono - interval_start_mono_us);
        if (start_epoch_us < 0) {
            start_epoch_us = 0;
        }
        time_t start_sec = (time_t)(start_epoch_us / 1000000LL);
        struct tm utc_time;
        if (gmtime_r(&start_sec, &utc_time) == NULL ||
            strftime(event_time, 40, "%Y-%m-%dT%H:%M:%SZ", &utc_time) == 0) {
            return ESP_FAIL;
        }
        *timing_quality = "synchronized";
        *uncertainty_ms = 100.0;
        return ESP_OK;
    }

    time_t now = time(NULL);
    if (now < 1577836800) {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t delta_us = now_mono - interval_start_mono_us;
    time_t start_sec = now - (time_t)(delta_us / 1000000LL);
    struct tm utc_time;
    if (gmtime_r(&start_sec, &utc_time) == NULL ||
        strftime(event_time, 40, "%Y-%m-%dT%H:%M:%SZ", &utc_time) == 0) {
        return ESP_FAIL;
    }
    *timing_quality = "estimated";
    *uncertainty_ms = 5000.0;
    return ESP_OK;
}

static void observation_enrich(cJSON *state, const dbmap_effective_t *cfg, bool include_diag)
{
    (void)cfg;
    (void)include_diag;
    if (state == NULL) {
        return;
    }

    cJSON *capabilities = cJSON_GetObjectItem(state, "capabilities");
    if (capabilities == NULL) {
        capabilities = cJSON_CreateObject();
        if (capabilities == NULL) {
            return;
        }
        cJSON_AddItemToObject(state, "capabilities", capabilities);
    }

    cJSON *observation = cJSON_CreateObject();
    if (observation == NULL) {
        return;
    }
    cJSON_AddBoolToObject(observation, "publisher", true);
    cJSON_AddNumberToObject(observation, "protocol_version", 1);
#if CONFIG_DBMAP_EAR_OBS_RMS_ACTIVITY
    cJSON_AddStringToObject(observation, "detector_kind", "rms_activity");
    cJSON_AddNumberToObject(observation, "detector_version", 1);
#else
    cJSON_AddStringToObject(observation, "detector_kind", "none");
    cJSON_AddNumberToObject(observation, "detector_version", 0);
#endif
    cJSON_AddItemToObject(capabilities, "observation", observation);
}

uint32_t dbmap_ear_observation_next_sequence(void)
{
    return s_seq.next;
}

uint32_t dbmap_ear_observation_seq_limit(void)
{
    return s_seq.limit;
}

esp_err_t dbmap_ear_observation_publish(
    const dbmap_effective_t *cfg,
    double signal_level_dbfs,
    double duration_ms,
    int64_t capture_timestamp_monotonic_us,
    int64_t interval_start_monotonic_us
)
{
    if (cfg == NULL || cfg->identity.node_id[0] == '\0' || cfg->topics.observation[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (signal_level_dbfs > 0.0 || duration_ms < 0.0 || capture_timestamp_monotonic_us < 0 ||
        interval_start_monotonic_us < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!dbmap_mqtt_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    char event_time[40];
    const char *timing_quality = "unsynchronized";
    double uncertainty_ms = 60000.0;
    esp_err_t time_err = format_event_time_for_interval_start(
        interval_start_monotonic_us,
        event_time,
        &timing_quality,
        &uncertainty_ms
    );
    if (time_err != ESP_OK) {
        ESP_LOGW(TAG, "refuse observation publish: wall clock not available (%s)", esp_err_to_name(time_err));
        return time_err == ESP_ERR_INVALID_STATE ? ESP_ERR_INVALID_STATE : time_err;
    }

    char observation_id[37];
    format_uuid_v4(observation_id);

    uint32_t sequence = 0;
    esp_err_t seq_err = seq_allocate_consumed(&sequence);
    if (seq_err != ESP_OK) {
        ESP_LOGE(TAG, "sequence allocate failed: %s", esp_err_to_name(seq_err));
        return seq_err;
    }

    dbmap_observation_fields_t fields = {
        .observation_id = observation_id,
        .node_id = cfg->identity.node_id,
        .sequence_number = sequence,
        .event_time_utc = event_time,
        .capture_timestamp_monotonic_us = capture_timestamp_monotonic_us,
        .timing_quality = timing_quality,
        .timestamp_uncertainty_ms = uncertainty_ms,
        .signal_level_dbfs = signal_level_dbfs,
        .duration_ms = duration_ms,
    };

    char *json = dbmap_observation_build_json(&fields);
    if (json == NULL) {
        ESP_LOGE(TAG, "observation JSON build failed (sequence %lu consumed)", (unsigned long)sequence);
        return ESP_FAIL;
    }

    esp_err_t pub_err = dbmap_mqtt_publish_json(cfg->topics.observation, json, 1);
    if (pub_err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "published observation_id=%s sequence=%lu level=%.2f duration_ms=%.0f",
            observation_id,
            (unsigned long)sequence,
            signal_level_dbfs,
            duration_ms
        );
    } else {
        ESP_LOGW(
            TAG,
            "observation publish failed (%s); id=%s sequence=%lu consumed (not queued)",
            esp_err_to_name(pub_err),
            observation_id,
            (unsigned long)sequence
        );
    }
    free(json);
    return pub_err;
}

static const char *emit_kind_name(dbmap_activity_emit_kind_t kind)
{
    switch (kind) {
    case DBMAP_ACTIVITY_EMIT_START:
        return "start";
    case DBMAP_ACTIVITY_EMIT_PERIODIC:
        return "periodic";
    case DBMAP_ACTIVITY_EMIT_END:
        return "end";
    default:
        return "none";
    }
}

#if CONFIG_DBMAP_EAR_OBS_RMS_ACTIVITY
static void handle_emission(const dbmap_activity_emission_t *emission)
{
    if (s_cfg == NULL || emission == NULL || emission->kind == DBMAP_ACTIVITY_EMIT_NONE) {
        return;
    }

    if (!dbmap_mqtt_is_connected()) {
        s_dropped_while_mqtt_down += 1;
        if (!s_logged_mqtt_drop) {
            s_logged_mqtt_drop = true;
            ESP_LOGW(
                TAG,
                "MQTT down: dropping %s emission (detection continues; no sequence consumed)",
                emit_kind_name(emission->kind)
            );
        }
        return;
    }
    s_logged_mqtt_drop = false;

    esp_err_t err = dbmap_ear_observation_publish(
        s_cfg,
        (double)emission->signal_level_dbfs,
        emission->duration_ms,
        emission->interval_end_mono_us,
        emission->interval_start_mono_us
    );
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "%s emission publish result: %s",
            emit_kind_name(emission->kind),
            esp_err_to_name(err)
        );
    }
}

static void activity_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "RMS activity task started");
    while (1) {
        dbmap_ear_mic_levels_t levels;
        if (dbmap_ear_mic_get_levels(&levels) == ESP_OK && levels.valid) {
            dbmap_activity_sample_t sample = {
                .has_measurement = true,
                .rms_dbfs = levels.rms_dbfs,
                .window_ms = levels.window_ms,
                .measured_at_us = levels.measured_at_us,
                .window_seq = levels.window_seq,
                .ms_since_i2s_enable = levels.ms_since_i2s_enable,
                .transport_status = levels.transport_status,
                .microphone_status = levels.microphone_status,
            };
            dbmap_activity_emission_t emission = dbmap_activity_detector_feed(&s_detector, &sample);
            handle_emission(&emission);
        }
#if CONFIG_DBMAP_EAR_OBS_TEST_PUBLISH_ONCE
        if (s_cfg != NULL) {
            dbmap_ear_observation_poll_test_trigger(s_cfg);
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif

esp_err_t dbmap_ear_observation_start(const dbmap_effective_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_OK;
    }

    uint32_t block_size = (uint32_t)CONFIG_DBMAP_EAR_OBS_SEQ_BLOCK_SIZE;
    uint32_t durable_limit = 0;
    esp_err_t err = seq_limit_load(&durable_limit, block_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sequence limit load failed (%s); using 0", esp_err_to_name(err));
        durable_limit = 0;
    }

    dbmap_obs_seq_init(&s_seq, durable_limit, block_size);
    err = seq_ensure_reserved();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "initial sequence reserve failed: %s", esp_err_to_name(err));
        return err;
    }

    s_cfg = cfg;
    dbmap_health_add_enricher(observation_enrich);

#if CONFIG_DBMAP_EAR_OBS_RMS_ACTIVITY
    dbmap_activity_config_t act_cfg = {
        .enter_threshold_dbfs = ((float)CONFIG_DBMAP_EAR_OBS_ENTER_THRESHOLD_DBFS_X1000) / 1000.0f,
        .exit_threshold_dbfs = ((float)CONFIG_DBMAP_EAR_OBS_EXIT_THRESHOLD_DBFS_X1000) / 1000.0f,
        .min_activity_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_MIN_ACTIVITY_MS,
        .exit_hold_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_EXIT_HOLD_MS,
        .boot_settle_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_BOOT_SETTLE_MS,
        .observation_interval_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_INTERVAL_MS,
        .max_invalid_hold_ms = (uint32_t)CONFIG_DBMAP_EAR_OBS_MAX_INVALID_HOLD_MS,
    };
    if (!dbmap_activity_config_valid(&act_cfg)) {
        ESP_LOGE(TAG, "invalid RMS activity config (check enter > exit thresholds)");
        return ESP_ERR_INVALID_ARG;
    }
    dbmap_activity_detector_init(&s_detector, &act_cfg);
    BaseType_t ok = xTaskCreate(activity_task, "ear_obs", 4096, NULL, 4, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(
        TAG,
        "RMS activity detector ready enter=%.1f exit=%.1f min_ms=%lu interval_ms=%lu",
        (double)act_cfg.enter_threshold_dbfs,
        (double)act_cfg.exit_threshold_dbfs,
        (unsigned long)act_cfg.min_activity_ms,
        (unsigned long)act_cfg.observation_interval_ms
    );
#endif

    s_started = true;
    ESP_LOGI(
        TAG,
        "observation publisher ready next_seq=%lu seq_limit=%lu block=%lu",
        (unsigned long)s_seq.next,
        (unsigned long)s_seq.limit,
        (unsigned long)s_seq.block_size
    );
    return ESP_OK;
}

void dbmap_ear_observation_poll_test_trigger(const dbmap_effective_t *cfg)
{
#if !CONFIG_DBMAP_EAR_OBS_TEST_PUBLISH_ONCE
    (void)cfg;
    return;
#else
    if (cfg == NULL || s_test_published || !s_started) {
        return;
    }
    if (!dbmap_mqtt_is_connected()) {
        if (!s_logged_wait_mqtt) {
            ESP_LOGI(TAG, "Phase 1 test Observation waiting for MQTT");
            s_logged_wait_mqtt = true;
        }
        return;
    }

    double signal_level_dbfs = ((double)CONFIG_DBMAP_EAR_OBS_TEST_SIGNAL_LEVEL_DBFS) / 1000.0;
    double duration_ms = (double)CONFIG_DBMAP_EAR_OBS_TEST_DURATION_MS;
    int64_t capture_us = (int64_t)esp_timer_get_time();
    int64_t start_us = capture_us;

    dbmap_ear_mic_levels_t levels;
    if (dbmap_ear_mic_get_levels(&levels) == ESP_OK && levels.valid) {
        signal_level_dbfs = (double)levels.rms_dbfs;
        if (signal_level_dbfs > 0.0) {
            signal_level_dbfs = 0.0;
        }
        duration_ms = (double)levels.window_ms;
        if (levels.measured_at_us > 0) {
            capture_us = levels.measured_at_us;
            start_us = levels.measured_at_us - ((int64_t)levels.window_ms * 1000LL);
            if (start_us < 0) {
                start_us = 0;
            }
        }
    }

    esp_err_t err = dbmap_ear_observation_publish(
        cfg,
        signal_level_dbfs,
        duration_ms,
        capture_us,
        start_us
    );
    if (err == ESP_OK) {
        s_test_published = true;
        ESP_LOGI(TAG, "Phase 1 test Observation published (one-shot complete)");
    } else if (!s_logged_wait_mqtt) {
        s_logged_wait_mqtt = true;
        ESP_LOGW(TAG, "Phase 1 test Observation publish deferred: %s", esp_err_to_name(err));
    }
#endif
}
