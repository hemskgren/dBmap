#include "dbmap_ear_observation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "dbmap_activity_detector.h"
#include "dbmap_acoustic_l0.h"
#include "dbmap_ear_mic.h"
#include "dbmap_health.h"
#include "dbmap_mqtt.h"
#include "dbmap_observation_lifecycle.h"
#include "dbmap_observation_seq.h"
#include "dbmap_ota.h"
#include "dbmap_time.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

/** Bounded PUBACK wait for OTA END only (normal Observations do not wait). */
static const uint32_t kOtaEndAckTimeoutMs = 5000;

#if CONFIG_DBMAP_EAR_OBS_L0
/* Static capacity from Kconfig; must cover configured PERIODIC interval (+2 edge). */
#define EAR_OBS_L0_HIST_CAP ((uint32_t)CONFIG_DBMAP_EAR_OBS_L0_MAX_WINDOWS)
#define EAR_OBS_L0_WINDOWS_NEEDED                                              \
    (((uint32_t)CONFIG_DBMAP_EAR_OBS_INTERVAL_MS / (uint32_t)CONFIG_DBMAP_EAR_MIC_WINDOW_MS) + 2u)
#endif

static const char *TAG = "ear_obs";

static const char *NVS_NS = "ear_obs";
static const char *NVS_KEY_SEQ_LIMIT = "seq_lim";
static const char *NVS_KEY_SEQ_LEGACY = "seq"; /* Phase 1 next-value; migrated as limit */

static const dbmap_effective_t *s_cfg;
static dbmap_obs_seq_t s_seq;
static bool s_started;
static bool s_test_published;
static bool s_logged_wait_mqtt;
static bool s_ota_close_registered;
static SemaphoreHandle_t s_lifecycle_mu;
static dbmap_obs_emit_gate_t s_emit_gate = DBMAP_OBS_GATE_RUN;
/** Set only when START publish_json returns ESP_OK (MQTT client accept, not PUBACK/Hub). */
static bool s_start_publish_accepted;

#if CONFIG_DBMAP_EAR_OBS_RMS_ACTIVITY
static bool s_logged_mqtt_drop;
static uint32_t s_dropped_while_mqtt_down;
static dbmap_activity_detector_t s_detector;

typedef struct {
    bool should_publish;
    char node_id[DBMAP_NODE_ID_MAX];
    char topic[DBMAP_TOPIC_MAX];
    char emission_kind[8];
    char termination_reason[16];
    double signal_level_dbfs;
    double duration_ms;
    int64_t interval_start_mono_us;
    int64_t interval_end_mono_us;
    bool has_measurements;
    dbmap_l0_interval_t measurements;
} ear_obs_end_snapshot_t;
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
static int64_t s_diag_last_snap_us;
static int64_t s_diag_last_counters_us;
#endif
#if CONFIG_DBMAP_EAR_OBS_L0
typedef struct {
    dbmap_l0_window_t window;
    uint8_t usable; /* detector usable gate at capture association time */
} ear_obs_l0_ring_entry_t;

static ear_obs_l0_ring_entry_t s_l0_ring[EAR_OBS_L0_HIST_CAP];
static uint32_t s_l0_ring_head;
static uint32_t s_l0_ring_count;
static uint32_t s_l0_last_window_seq;
static float s_l0_hist_rms[EAR_OBS_L0_HIST_CAP];
static float s_l0_hist_hl[EAR_OBS_L0_HIST_CAP];
static uint8_t s_l0_hist_hl_valid[EAR_OBS_L0_HIST_CAP];
static int64_t s_l0_hist_t_end[EAR_OBS_L0_HIST_CAP];
static float s_l0_hist_crest[EAR_OBS_L0_HIST_CAP];
static uint8_t s_l0_hist_crest_ok[EAR_OBS_L0_HIST_CAP];
static dbmap_l0_interval_t s_l0_last_interval;
static bool s_l0_last_interval_valid;
static bool s_l0_capacity_ok;
static uint32_t s_l0_hist_overflow;
#endif
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
    int64_t interval_start_monotonic_us,
    const char *emission_kind,
    const char *termination_reason,
    const dbmap_l0_interval_t *measurements
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

#if CONFIG_DBMAP_EAR_OBS_PUBLISH_L0
    /* Extended v1: never publish without emission_kind + L0 measurements. */
    if (emission_kind == NULL || measurements == NULL) {
        ESP_LOGE(TAG, "L0 publish refuse: missing emission_kind or measurements");
        return ESP_ERR_INVALID_STATE;
    }
    if (measurements->history_overflow) {
        ESP_LOGE(TAG, "L0 publish refuse: history overflow (incomplete T1–T6)");
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_DBMAP_EAR_OBS_L0
    if (!s_l0_capacity_ok) {
        ESP_LOGE(TAG, "L0 publish refuse: interval exceeds L0 history capacity");
        return ESP_ERR_INVALID_STATE;
    }
#endif
    {
        dbmap_ear_mic_l0_perf_t perf;
        if (dbmap_ear_mic_get_l0_perf(&perf) != ESP_OK || !perf.enabled || !perf.pcm_ready) {
            ESP_LOGE(TAG, "L0 publish refuse: L0 analysis unavailable");
            return ESP_ERR_INVALID_STATE;
        }
    }
#else
    (void)emission_kind;
    (void)measurements;
    (void)termination_reason;
#endif

#if CONFIG_DBMAP_EAR_OBS_PUBLISH_L0
    if (termination_reason != NULL
        && (emission_kind == NULL || strcmp(emission_kind, "end") != 0)) {
        ESP_LOGE(TAG, "termination_reason refuse: only valid on end");
        return ESP_ERR_INVALID_ARG;
    }
#else
    termination_reason = NULL;
#endif

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
#if CONFIG_DBMAP_EAR_OBS_PUBLISH_L0
        .emission_kind = emission_kind,
        .termination_reason = termination_reason,
        .measurements = measurements,
#else
        .emission_kind = NULL,
        .termination_reason = NULL,
        .measurements = NULL,
#endif
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
            "published observation_id=%s sequence=%lu level=%.2f duration_ms=%.0f kind=%s l0=%d",
            observation_id,
            (unsigned long)sequence,
            signal_level_dbfs,
            duration_ms,
            emission_kind != NULL ? emission_kind : "-",
            fields.measurements != NULL ? 1 : 0
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

#if CONFIG_DBMAP_EAR_OBS_L0
static void l0_ring_push(const dbmap_l0_window_t *window, bool usable)
{
    if (window == NULL || !window->valid) {
        return;
    }
    ear_obs_l0_ring_entry_t *slot = &s_l0_ring[s_l0_ring_head];
    slot->window = *window;
    slot->usable = usable ? 1u : 0u;
    s_l0_ring_head = (s_l0_ring_head + 1u) % EAR_OBS_L0_HIST_CAP;
    if (s_l0_ring_count < EAR_OBS_L0_HIST_CAP) {
        s_l0_ring_count++;
    }
}

static void l0_finalize_for_emission(const dbmap_activity_emission_t *emission)
{
    if (emission == NULL || emission->kind == DBMAP_ACTIVITY_EMIT_NONE) {
        return;
    }

    dbmap_l0_interval_accum_t acc;
    dbmap_l0_interval_accum_init(
        &acc,
        (uint32_t)CONFIG_DBMAP_EAR_MIC_SAMPLE_RATE_HZ,
        (uint32_t)CONFIG_DBMAP_EAR_MIC_WINDOW_MS,
        0,
        emission->interval_start_mono_us,
        emission->interval_end_mono_us,
        s_l0_hist_rms,
        s_l0_hist_hl,
        s_l0_hist_hl_valid,
        s_l0_hist_t_end,
        s_l0_hist_crest,
        s_l0_hist_crest_ok,
        EAR_OBS_L0_HIST_CAP
    );

    /* Walk oldest→newest in the ring. */
    const uint32_t n = s_l0_ring_count;
    const uint32_t start = (s_l0_ring_head + EAR_OBS_L0_HIST_CAP - n) % EAR_OBS_L0_HIST_CAP;
    for (uint32_t i = 0; i < n; ++i) {
        const ear_obs_l0_ring_entry_t *e = &s_l0_ring[(start + i) % EAR_OBS_L0_HIST_CAP];
        if (e->usable == 0) {
            continue;
        }
        dbmap_l0_interval_accum_add(&acc, &e->window);
    }

    dbmap_l0_interval_t iv;
    dbmap_l0_interval_accum_finalize(&acc, &iv);
    if (iv.history_overflow) {
        s_l0_hist_overflow += 1;
        ESP_LOGW(
            TAG,
            "L0 history overflow kind=%s win=%lu cap=%lu (T1–T6 cleared)",
            emit_kind_name(emission->kind),
            (unsigned long)iv.window_count,
            (unsigned long)EAR_OBS_L0_HIST_CAP
        );
    }
    s_l0_last_interval = iv;
    s_l0_last_interval_valid = true;

#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
    ESP_LOGI(
        TAG,
        "L0 interval kind=%s win=%lu clipped_win=%lu "
        "rms_mean=%.2f rms_min=%.2f rms_max=%.2f "
        "B0=%.2f B1=%.2f B2=%.2f B3=%.2f "
        "f0=%.3f f1=%.3f f2=%.3f f3=%.3f "
        "hl_mean=%s%.2f "
        "T1_stdev=%.3f T1_range=%.3f T2=%.4f "
        "T3_crest_max=%.2f T3_hi_rate=%.3f n_crest=%lu "
        "T6=%.4f t=(%lld,%lld] us",
        emit_kind_name(emission->kind),
        (unsigned long)iv.window_count,
        (unsigned long)iv.clipped_window_count,
        iv.has_rms_stats ? (double)iv.rms_mean_dbfs : -120.0,
        iv.has_rms_stats ? (double)iv.rms_min_dbfs : -120.0,
        iv.has_rms_stats ? (double)iv.rms_max_dbfs : -120.0,
        iv.has_band_means ? (double)iv.band_mean_dbfs[0] : -120.0,
        iv.has_band_means ? (double)iv.band_mean_dbfs[1] : -120.0,
        iv.has_band_means ? (double)iv.band_mean_dbfs[2] : -120.0,
        iv.has_band_means ? (double)iv.band_mean_dbfs[3] : -120.0,
        iv.has_band_fractions ? (double)iv.band_fraction[0] : 0.0,
        iv.has_band_fractions ? (double)iv.band_fraction[1] : 0.0,
        iv.has_band_fractions ? (double)iv.band_fraction[2] : 0.0,
        iv.has_band_fractions ? (double)iv.band_fraction[3] : 0.0,
        iv.has_hl_mean ? "" : "n/a ",
        iv.has_hl_mean ? (double)iv.hl_mean_db : 0.0,
        iv.has_t1 ? (double)iv.rms_stdev_db : 0.0,
        iv.has_t1 ? (double)iv.rms_range_db : 0.0,
        iv.has_t2 ? (double)iv.rms_slope_db_per_s : 0.0,
        iv.has_t3 ? (double)iv.crest_max_db : 0.0,
        iv.has_t3 ? (double)iv.high_crest_rate : 0.0,
        (unsigned long)iv.n_crest,
        iv.has_t6 ? (double)iv.hl_slope_db_per_s : 0.0,
        (long long)iv.interval_start_mono_us,
        (long long)iv.interval_end_mono_us
    );
#else
    (void)iv;
#endif
}

static void l0_on_new_window(const dbmap_ear_mic_levels_t *levels, bool levels_ok)
{
    (void)dbmap_ear_mic_l0_poll();

    if (!levels_ok || levels == NULL || !levels->valid) {
        return;
    }
    if (levels->window_seq == 0 || levels->window_seq == s_l0_last_window_seq) {
        return;
    }

    dbmap_l0_window_t l0w;
    if (dbmap_ear_mic_get_l0_window(&l0w) != ESP_OK || !l0w.valid) {
        return;
    }
    /* Require L0 end time to match this RMS window (avoids pushing stale L0). */
    if (l0w.window_end_mono_us != levels->measured_at_us) {
        return;
    }
    s_l0_last_window_seq = levels->window_seq;

    dbmap_activity_sample_t gate = {
        .has_measurement = true,
        .rms_dbfs = levels->rms_dbfs,
        .window_ms = levels->window_ms,
        .measured_at_us = levels->measured_at_us,
        .window_seq = levels->window_seq,
        .ms_since_i2s_enable = levels->ms_since_i2s_enable,
        .transport_status = levels->transport_status,
        .microphone_status = levels->microphone_status,
    };
    const bool usable = dbmap_activity_sample_usable(&gate);
    l0_ring_push(&l0w, usable);
}
#endif /* CONFIG_DBMAP_EAR_OBS_L0 */

static const char *termination_reason_for_emission(const dbmap_activity_emission_t *emission)
{
    if (emission == NULL || emission->kind != DBMAP_ACTIVITY_EMIT_END) {
        return NULL;
    }
    switch (emission->termination) {
    case DBMAP_ACTIVITY_TERM_INVALID_HOLD:
        return "invalid_hold";
    case DBMAP_ACTIVITY_TERM_NATURAL:
        return "natural";
    default:
        return "natural";
    }
}

static void handle_emission(const dbmap_activity_emission_t *emission)
{
    if (s_cfg == NULL || emission == NULL || emission->kind == DBMAP_ACTIVITY_EMIT_NONE) {
        return;
    }

    /* Episode closed in detector RAM even if MQTT drops the END. */
    if (emission->kind == DBMAP_ACTIVITY_EMIT_END) {
        s_start_publish_accepted = false;
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

    const dbmap_l0_interval_t *meas = NULL;
#if CONFIG_DBMAP_EAR_OBS_L0
    if (s_l0_last_interval_valid) {
        meas = &s_l0_last_interval;
    }
#endif
    const char *term = NULL;
#if CONFIG_DBMAP_EAR_OBS_PUBLISH_L0
    term = termination_reason_for_emission(emission);
#endif
    esp_err_t err = dbmap_ear_observation_publish(
        s_cfg,
        (double)emission->signal_level_dbfs,
        emission->duration_ms,
        emission->interval_end_mono_us,
        emission->interval_start_mono_us,
        emit_kind_name(emission->kind),
        term,
        meas
    );
    if (emission->kind == DBMAP_ACTIVITY_EMIT_START && err == ESP_OK) {
        /* MQTT client accepted publish — not broker PUBACK or Hub ingest. */
        s_start_publish_accepted = true;
    }
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "%s emission publish result: %s",
            emit_kind_name(emission->kind),
            esp_err_to_name(err)
        );
    }
}

#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
static const char *activity_state_name(dbmap_activity_state_t st)
{
    switch (st) {
    case DBMAP_ACTIVITY_IDLE:
        return "IDLE";
    case DBMAP_ACTIVITY_PENDING_ENTER:
        return "PENDING_ENTER";
    case DBMAP_ACTIVITY_ACTIVE:
        return "ACTIVE";
    case DBMAP_ACTIVITY_PENDING_EXIT:
        return "PENDING_EXIT";
    default:
        return "unknown";
    }
}

/**
 * Rate-limited console diagnostics. Runs only from activity_task (owns
 * s_detector / drop counter). Mic levels are already snapshotted by
 * dbmap_ear_mic_get_levels under its critical section. Does not block I2S.
 */
static void activity_serial_diag(const dbmap_ear_mic_levels_t *levels, bool levels_ok)
{
    const int64_t now_us = esp_timer_get_time();

    if (s_diag_last_snap_us == 0 || (now_us - s_diag_last_snap_us) >= 1000000LL) {
        s_diag_last_snap_us = now_us;

        bool usable = false;
        if (levels_ok && levels->valid) {
            dbmap_activity_sample_t gate = {
                .has_measurement = true,
                .rms_dbfs = levels->rms_dbfs,
                .window_ms = levels->window_ms,
                .measured_at_us = levels->measured_at_us,
                .window_seq = levels->window_seq,
                .ms_since_i2s_enable = levels->ms_since_i2s_enable,
                .transport_status = levels->transport_status,
                .microphone_status = levels->microphone_status,
            };
            usable = dbmap_activity_sample_usable(&gate);
        }

        if (levels_ok && levels->valid) {
            ESP_LOGI(
                TAG,
                "diag snap mono_us=%lld rms_dbfs=%.2f peak_dbfs=%.2f xport=%s mic=%s "
                "state=%s enter=%.1f exit=%.1f win_valid=1 usable=%d win_seq=%lu",
                (long long)now_us,
                (double)levels->rms_dbfs,
                (double)levels->peak_dbfs,
                levels->transport_status[0] ? levels->transport_status : "unavailable",
                levels->microphone_status[0] ? levels->microphone_status : "unavailable",
                activity_state_name(dbmap_activity_detector_state(&s_detector)),
                (double)s_detector.cfg.enter_threshold_dbfs,
                (double)s_detector.cfg.exit_threshold_dbfs,
                usable ? 1 : 0,
                (unsigned long)levels->window_seq
            );
        } else {
            ESP_LOGI(
                TAG,
                "diag snap mono_us=%lld rms_dbfs=n/a peak_dbfs=n/a xport=n/a mic=n/a "
                "state=%s enter=%.1f exit=%.1f win_valid=0 usable=0",
                (long long)now_us,
                activity_state_name(dbmap_activity_detector_state(&s_detector)),
                (double)s_detector.cfg.enter_threshold_dbfs,
                (double)s_detector.cfg.exit_threshold_dbfs
            );
        }

#if CONFIG_DBMAP_EAR_OBS_L0
        {
            dbmap_l0_window_t l0w;
            if (dbmap_ear_mic_get_l0_window(&l0w) == ESP_OK && l0w.valid) {
                ESP_LOGI(
                    TAG,
                    "L0 win rms=%.2f peak=%.2f crest=%.2f clip=%d "
                    "B0=%.2f B1=%.2f B2=%.2f B3=%.2f hl=%s%.2f "
                    "n=%lu end_us=%lld",
                    (double)l0w.rms_dbfs,
                    (double)l0w.peak_dbfs,
                    (double)l0w.crest_db,
                    l0w.clipped ? 1 : 0,
                    (double)l0w.band_dbfs[0],
                    (double)l0w.band_dbfs[1],
                    (double)l0w.band_dbfs[2],
                    (double)l0w.band_dbfs[3],
                    l0w.hl_valid ? "" : "n/a ",
                    l0w.hl_valid ? (double)l0w.hl_db : 0.0,
                    (unsigned long)l0w.sample_count,
                    (long long)l0w.window_end_mono_us
                );
            }
        }
#endif
    }

    if (s_diag_last_counters_us == 0 || (now_us - s_diag_last_counters_us) >= 5000000LL) {
        s_diag_last_counters_us = now_us;
        ESP_LOGI(
            TAG,
            "diag counters start=%lu periodic=%lu end=%lu invalid=%lu "
            "forced_end_invalid=%lu mqtt_drop=%lu windows=%lu boot_settle=%lu",
            (unsigned long)s_detector.emissions_start,
            (unsigned long)s_detector.emissions_periodic,
            (unsigned long)s_detector.emissions_end,
            (unsigned long)s_detector.windows_invalid,
            (unsigned long)s_detector.forced_ends_invalid,
            (unsigned long)s_dropped_while_mqtt_down,
            (unsigned long)s_detector.windows_seen,
            (unsigned long)s_detector.windows_boot_settle
        );
#if CONFIG_DBMAP_EAR_OBS_L0
        {
            dbmap_ear_mic_l0_perf_t perf;
            if (dbmap_ear_mic_get_l0_perf(&perf) == ESP_OK) {
                ESP_LOGI(
                    TAG,
                    "L0 perf analyze_n=%lu us_last=%lu us_max=%lu overwrite=%lu "
                    "pcm_cap=%lu pcm_bytes=%lu free_heap_init=%lu free_spiram_init=%lu "
                    "ring=%lu/%lu",
                    (unsigned long)perf.analyze_count,
                    (unsigned long)perf.analyze_us_last,
                    (unsigned long)perf.analyze_us_max,
                    (unsigned long)perf.pending_overwrite,
                    (unsigned long)perf.pcm_cap_samples,
                    (unsigned long)perf.pcm_bytes_total,
                    (unsigned long)perf.free_heap_after_init,
                    (unsigned long)perf.free_spiram_after_init,
                    (unsigned long)s_l0_ring_count,
                    (unsigned long)EAR_OBS_L0_HIST_CAP
                );
            }
        }
#endif
    }
}
#endif

static void activity_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "RMS activity task started");
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
    ESP_LOGI(TAG, "serial diagnostics enabled (~1 Hz snap, ~5 s counters)");
#endif
#if CONFIG_DBMAP_EAR_OBS_L0
    ESP_LOGI(
        TAG,
        "L0 interval accumulate enabled hist_cap=%lu",
        (unsigned long)EAR_OBS_L0_HIST_CAP
    );
#endif
    while (1) {
        if (!dbmap_obs_gate_allows_feed(s_emit_gate)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        dbmap_ear_mic_levels_t levels;
        const bool levels_ok = (dbmap_ear_mic_get_levels(&levels) == ESP_OK);
        dbmap_activity_emission_t emission;
        memset(&emission, 0, sizeof(emission));
        emission.kind = DBMAP_ACTIVITY_EMIT_NONE;

        if (s_lifecycle_mu != NULL) {
            xSemaphoreTake(s_lifecycle_mu, portMAX_DELAY);
        }
        /* Recheck after lock: OTA may have entered CLOSING/CLOSED. */
        if (!dbmap_obs_gate_allows_feed(s_emit_gate)) {
            if (s_lifecycle_mu != NULL) {
                xSemaphoreGive(s_lifecycle_mu);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (levels_ok && levels.valid) {
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
            /* Detector first — L0 must not affect enter/exit decisions. */
            emission = dbmap_activity_detector_feed(&s_detector, &sample);
        }
#if CONFIG_DBMAP_EAR_OBS_L0
        /* Push before finalize so the closing window is in the ring. Membership
         * is start < window_end <= end, so that closing window is associated
         * with this emission (not the next). Still after feed — L0 is not on
         * the detector decision path. */
        l0_on_new_window(&levels, levels_ok);
        if (emission.kind != DBMAP_ACTIVITY_EMIT_NONE) {
            l0_finalize_for_emission(&emission);
        }
#endif
        handle_emission(&emission);
        if (s_lifecycle_mu != NULL) {
            xSemaphoreGive(s_lifecycle_mu);
        }
#if CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG
        activity_serial_diag(&levels, levels_ok);
#endif
#if CONFIG_DBMAP_EAR_OBS_TEST_PUBLISH_ONCE
        if (s_cfg != NULL) {
            dbmap_ear_observation_poll_test_trigger(s_cfg);
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * Publish a detached OTA END snapshot. Must not read detector/L0/gate/cfg globals.
 * Uses wall-clock, UUID, sequence allocate, and MQTT wait only.
 */
static esp_err_t publish_ota_end_snapshot(const ear_obs_end_snapshot_t *snap)
{
    if (snap == NULL || !snap->should_publish) {
        return ESP_OK;
    }
    if (snap->node_id[0] == '\0' || snap->topic[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (!dbmap_mqtt_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_DBMAP_EAR_OBS_PUBLISH_L0
    if (!snap->has_measurements || snap->measurements.history_overflow) {
        ESP_LOGE(TAG, "OTA END refuse: L0 measurements missing or overflow (snapshot)");
        return ESP_ERR_INVALID_STATE;
    }
#endif

    char event_time[40];
    const char *timing_quality = "unsynchronized";
    double uncertainty_ms = 60000.0;
    esp_err_t time_err = format_event_time_for_interval_start(
        snap->interval_start_mono_us,
        event_time,
        &timing_quality,
        &uncertainty_ms
    );
    if (time_err != ESP_OK) {
        return time_err == ESP_ERR_INVALID_STATE ? ESP_ERR_INVALID_STATE : time_err;
    }

    char observation_id[37];
    format_uuid_v4(observation_id);

    uint32_t sequence = 0;
    esp_err_t seq_err = seq_allocate_consumed(&sequence);
    if (seq_err != ESP_OK) {
        return seq_err;
    }

    dbmap_observation_fields_t fields = {
#if CONFIG_DBMAP_EAR_OBS_PUBLISH_L0
        .emission_kind = snap->emission_kind,
        .termination_reason = snap->termination_reason,
        .measurements = &snap->measurements,
#else
        .emission_kind = NULL,
        .termination_reason = NULL,
        .measurements = NULL,
#endif
        .observation_id = observation_id,
        .node_id = snap->node_id,
        .sequence_number = sequence,
        .event_time_utc = event_time,
        .capture_timestamp_monotonic_us = snap->interval_end_mono_us,
        .timing_quality = timing_quality,
        .timestamp_uncertainty_ms = uncertainty_ms,
        .signal_level_dbfs = snap->signal_level_dbfs,
        .duration_ms = snap->duration_ms,
    };

    char *json = dbmap_observation_build_json(&fields);
    if (json == NULL) {
        ESP_LOGE(TAG, "OTA END JSON build failed (sequence %lu consumed)", (unsigned long)sequence);
        return ESP_FAIL;
    }

    esp_err_t pub_err =
        dbmap_mqtt_publish_json_wait(snap->topic, json, 1, kOtaEndAckTimeoutMs);
    free(json);
    return pub_err;
}

static esp_err_t ear_observation_close_for_ota(void)
{
    /* Static: L0-bearing snapshot is too large for the main/OTA call stack. */
    static ear_obs_end_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    bool attempted_end = false;

    if (!s_started || s_lifecycle_mu == NULL) {
        ESP_LOGI(TAG, "OTA acoustic close: observation not started; nothing to close");
        return ESP_OK;
    }

    xSemaphoreTake(s_lifecycle_mu, portMAX_DELAY);
    s_emit_gate = DBMAP_OBS_GATE_CLOSING;

    const dbmap_activity_state_t state = dbmap_activity_detector_state(&s_detector);
    const dbmap_obs_ota_action_t action =
        dbmap_obs_ota_action_for_state(state, s_start_publish_accepted);

    if (action == DBMAP_OBS_OTA_ACTION_FORCE_END) {
        dbmap_activity_emission_t emission = dbmap_activity_detector_force_end(&s_detector);
        if (emission.kind == DBMAP_ACTIVITY_EMIT_END) {
#if CONFIG_DBMAP_EAR_OBS_L0
            l0_finalize_for_emission(&emission);
#endif
            snap.should_publish = true;
            attempted_end = true;
            strncpy(snap.emission_kind, "end", sizeof(snap.emission_kind) - 1);
            strncpy(snap.termination_reason, "ota_reboot", sizeof(snap.termination_reason) - 1);
            snap.signal_level_dbfs = (double)emission.signal_level_dbfs;
            snap.duration_ms = emission.duration_ms;
            snap.interval_start_mono_us = emission.interval_start_mono_us;
            snap.interval_end_mono_us = emission.interval_end_mono_us;
#if CONFIG_DBMAP_EAR_OBS_L0
            if (s_l0_last_interval_valid) {
                snap.measurements = s_l0_last_interval;
                snap.has_measurements = true;
            }
#endif
            if (s_cfg != NULL) {
                strncpy(snap.node_id, s_cfg->identity.node_id, sizeof(snap.node_id) - 1);
                strncpy(snap.topic, s_cfg->topics.observation, sizeof(snap.topic) - 1);
            }
        } else {
            dbmap_activity_detector_reset_idle(&s_detector);
        }
        s_start_publish_accepted = false;
    } else if (action == DBMAP_OBS_OTA_ACTION_SILENT_ABORT) {
        dbmap_activity_detector_reset_idle(&s_detector);
        s_start_publish_accepted = false;
        ESP_LOGI(
            TAG,
            "OTA acoustic close: silent abort (state had no MQTT-accepted START)"
        );
    } else {
        ESP_LOGI(TAG, "OTA acoustic close: idle; no episode");
    }

    s_emit_gate = DBMAP_OBS_GATE_CLOSED;
    xSemaphoreGive(s_lifecycle_mu);

    /* PUBACK wait outside the lifecycle mutex. */
    const bool mqtt_was_connected = dbmap_mqtt_is_connected();
    esp_err_t pub_err = ESP_OK;
    dbmap_obs_mqtt_wait_result_t wait_result = DBMAP_OBS_MQTT_WAIT_FAILED;
    if (snap.should_publish) {
        if (!mqtt_was_connected) {
            ESP_LOGW(TAG, "OTA END not published: MQTT disconnected");
        } else {
            pub_err = publish_ota_end_snapshot(&snap);
            if (pub_err == ESP_OK) {
                wait_result = DBMAP_OBS_MQTT_WAIT_OK;
            } else if (pub_err == ESP_ERR_TIMEOUT) {
                wait_result = DBMAP_OBS_MQTT_WAIT_TIMEOUT;
            } else {
                wait_result = DBMAP_OBS_MQTT_WAIT_FAILED;
            }
        }
    }

    dbmap_obs_ota_pub_outcome_t outcome = dbmap_obs_ota_classify_publish(
        attempted_end, mqtt_was_connected, wait_result
    );
    ESP_LOGI(
        TAG,
        "OTA acoustic close done outcome=%s (PUBACK != Hub ingest)",
        dbmap_obs_ota_pub_outcome_name(outcome)
    );
    /* OTA reboot proceeds regardless of END publish result. */
    (void)pub_err;
    return ESP_OK;
}
#endif /* CONFIG_DBMAP_EAR_OBS_RMS_ACTIVITY — activity_task + close helpers */

#if !CONFIG_DBMAP_EAR_OBS_RMS_ACTIVITY
static esp_err_t ear_observation_close_for_ota(void)
{
    ESP_LOGI(TAG, "OTA acoustic close: RMS activity disabled; nothing to close");
    return ESP_OK;
}
#endif

esp_err_t dbmap_ear_observation_register_ota_close(void)
{
    if (s_ota_close_registered) {
        return ESP_OK;
    }
    if (s_lifecycle_mu == NULL) {
        s_lifecycle_mu = xSemaphoreCreateMutex();
        if (s_lifecycle_mu == NULL) {
            ESP_LOGE(TAG, "OTA close register failed: no mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t err = dbmap_ota_register_acoustic_close(ear_observation_close_for_ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA close register failed: %s", esp_err_to_name(err));
        return err;
    }
    s_emit_gate = DBMAP_OBS_GATE_RUN;
    s_ota_close_registered = true;
    ESP_LOGI(TAG, "OTA acoustic-close callback registered");
    return ESP_OK;
}

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
#if CONFIG_DBMAP_EAR_OBS_L0
    s_l0_ring_head = 0;
    s_l0_ring_count = 0;
    s_l0_last_window_seq = 0;
    s_l0_last_interval_valid = false;
    s_l0_hist_overflow = 0;
    memset(&s_l0_last_interval, 0, sizeof(s_l0_last_interval));
    s_l0_capacity_ok = (EAR_OBS_L0_WINDOWS_NEEDED <= EAR_OBS_L0_HIST_CAP);
    if (!s_l0_capacity_ok) {
        ESP_LOGE(
            TAG,
            "L0 capacity too small: need %lu windows for interval_ms=%d window_ms=%d, have cap=%lu. "
            "Raise DBMAP_EAR_OBS_L0_MAX_WINDOWS or lower INTERVAL_MS.",
            (unsigned long)EAR_OBS_L0_WINDOWS_NEEDED,
            CONFIG_DBMAP_EAR_OBS_INTERVAL_MS,
            CONFIG_DBMAP_EAR_MIC_WINDOW_MS,
            (unsigned long)EAR_OBS_L0_HIST_CAP
        );
#if CONFIG_DBMAP_EAR_OBS_PUBLISH_L0
        return ESP_ERR_INVALID_SIZE;
#endif
    } else {
        ESP_LOGI(
            TAG,
            "L0 capacity ok need=%lu cap=%lu (~%lu bytes ring)",
            (unsigned long)EAR_OBS_L0_WINDOWS_NEEDED,
            (unsigned long)EAR_OBS_L0_HIST_CAP,
            (unsigned long)(EAR_OBS_L0_HIST_CAP * sizeof(ear_obs_l0_ring_entry_t))
        );
    }
#endif
    /* Extra stack for L0 interval diag formatting when SERIAL_DIAG is on. */
    BaseType_t ok = xTaskCreate(activity_task, "ear_obs", 6144, NULL, 4, NULL);
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
#if CONFIG_DBMAP_EAR_OBS_PUBLISH_L0
    ESP_LOGI(TAG, "observation protocol v1 + L0 measurements (emission_kind required)");
#else
    ESP_LOGI(TAG, "observation protocol v1 legacy (enable DBMAP_EAR_OBS_PUBLISH_L0 after Hub ready)");
#endif
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

#if CONFIG_DBMAP_EAR_OBS_PUBLISH_L0
    /* One-shot test path has no L0 aggregate; incompatible with L0 MQTT publish. */
    ESP_LOGW(TAG, "Phase 1 test Observation disabled when PUBLISH_L0 is enabled");
    s_test_published = true;
    return;
#else
    esp_err_t err = dbmap_ear_observation_publish(
        cfg,
        signal_level_dbfs,
        duration_ms,
        capture_us,
        start_us,
        NULL,
        NULL,
        NULL
    );
    if (err == ESP_OK) {
        s_test_published = true;
        ESP_LOGI(TAG, "Phase 1 test Observation published (one-shot complete)");
    } else if (!s_logged_wait_mqtt) {
        s_logged_wait_mqtt = true;
        ESP_LOGW(TAG, "Phase 1 test Observation publish deferred: %s", esp_err_to_name(err));
    }
#endif
#endif
}
