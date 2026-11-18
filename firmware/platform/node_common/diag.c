#include "sp_diag.h"

#include <stdint.h>
#include <string.h>

#include "sp_diag_memory.h"
#include "sp_lifecycle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG_SP = "sp";
static const char *TAG_MEM = "sp_mem";

#ifndef CONFIG_DBMAP_DIAG_ENABLED
#define CONFIG_DBMAP_DIAG_ENABLED 1
#endif
#ifndef CONFIG_DBMAP_DIAG_RSSI_DEGRADED_DBM
#define CONFIG_DBMAP_DIAG_RSSI_DEGRADED_DBM (-80)
#endif
#ifndef CONFIG_DBMAP_DIAG_COUNTER_SATURATE
#define CONFIG_DBMAP_DIAG_COUNTER_SATURATE 999
#endif

typedef struct {
    uint16_t wifi_disconnects;
    uint16_t mqtt_disconnects;
    uint16_t mqtt_reconnects;
    uint16_t mqtt_connect_attempts;
    uint16_t mqtt_connect_failures;
    uint16_t desired_failures;
    uint16_t mqtt_observation_start_rejected_count;
    uint16_t mqtt_observation_periodic_rejected_count;
    uint16_t mqtt_observation_end_rejected_count;
    uint16_t mqtt_observation_complete_rejected_count;
    uint16_t mqtt_observation_skipped_disconnected_count;
    char last_reset_reason[24];
    bool reset_reason_pending;
    bool mqtt_was_connected;
} sp_diag_state_t;

static sp_diag_config_t s_cfg;
static sp_diag_state_t s_state;
static sp_diag_mem_reporter_t s_mem_rep;
static uint32_t s_mem_pending_fp;
static bool s_mem_pending;

typedef struct {
    char name[16];
    TaskHandle_t handle;
    bool used;
} sp_diag_tracked_task_t;

static sp_diag_tracked_task_t s_tracked_tasks[SP_DIAG_TRACKED_TASKS];

static void saturate_inc(uint16_t *counter)
{
    if (*counter < s_cfg.counter_saturate) {
        (*counter)++;
    }
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "POWERON";
    case ESP_RST_EXT:
        return "EXT";
    case ESP_RST_SW:
        return "SW";
    case ESP_RST_PANIC:
        return "PANIC";
    case ESP_RST_INT_WDT:
        return "INT_WDT";
    case ESP_RST_TASK_WDT:
        return "TASK_WDT";
    case ESP_RST_WDT:
        return "WDT";
    case ESP_RST_DEEPSLEEP:
        return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
        return "BROWNOUT";
    case ESP_RST_SDIO:
        return "SDIO";
    default:
        return "UNKNOWN";
    }
}

void sp_diag_config_defaults(sp_diag_config_t *out)
{
    if (out == NULL) {
        return;
    }
    out->enabled = CONFIG_DBMAP_DIAG_ENABLED != 0;
    out->rssi_degraded_dbm = CONFIG_DBMAP_DIAG_RSSI_DEGRADED_DBM;
    out->counter_saturate = (uint16_t)CONFIG_DBMAP_DIAG_COUNTER_SATURATE;
    if (out->counter_saturate == 0) {
        out->counter_saturate = 999;
    }
}

void sp_diag_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    sp_diag_config_defaults(&s_cfg);
    sp_diag_mem_reporter_init(&s_mem_rep);
    s_mem_pending_fp = 0u;
    s_mem_pending = false;
    const char *reason = reset_reason_name(esp_reset_reason());
    strncpy(s_state.last_reset_reason, reason, sizeof(s_state.last_reset_reason) - 1);
    s_state.reset_reason_pending = true;
}

void sp_diag_apply_desired_payload(const cJSON *payload)
{
    if (!cJSON_IsObject(payload)) {
        return;
    }
    const cJSON *diag = cJSON_GetObjectItemCaseSensitive(payload, "diag");
    if (!cJSON_IsObject(diag)) {
        return;
    }

    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(diag, "enabled");
    if (cJSON_IsBool(enabled)) {
        s_cfg.enabled = cJSON_IsTrue(enabled);
    }

    const cJSON *rssi = cJSON_GetObjectItemCaseSensitive(diag, "rssi_degraded_dbm");
    if (cJSON_IsNumber(rssi)) {
        s_cfg.rssi_degraded_dbm = (int)rssi->valuedouble;
    }

    const cJSON *saturate = cJSON_GetObjectItemCaseSensitive(diag, "counter_saturate");
    if (cJSON_IsNumber(saturate) && saturate->valuedouble > 0 && saturate->valuedouble <= 65535) {
        s_cfg.counter_saturate = (uint16_t)saturate->valuedouble;
    }
}

const sp_diag_config_t *sp_diag_config(void)
{
    return &s_cfg;
}

void sp_diag_note_wifi_disconnect(void)
{
    if (sp_lifecycle_ota_reboot_pending()) {
        return;
    }
    saturate_inc(&s_state.wifi_disconnects);
}

void sp_diag_note_mqtt_disconnect(void)
{
    if (sp_lifecycle_ota_reboot_pending()) {
        /* Still clear local connected latch so a late CONNECTED before reboot
         * cannot look like a reconnect after an uncounted disconnect. */
        s_state.mqtt_was_connected = false;
        return;
    }
    /* Only count connected → disconnected. Duplicate callbacks are ignored. */
    if (s_state.mqtt_was_connected) {
        saturate_inc(&s_state.mqtt_disconnects);
        s_state.mqtt_was_connected = false;
    }
}

void sp_diag_note_mqtt_reconnect(void)
{
    if (sp_lifecycle_ota_reboot_pending()) {
        return;
    }
    if (!s_state.mqtt_was_connected) {
        if (s_state.mqtt_disconnects > 0) {
            saturate_inc(&s_state.mqtt_reconnects);
        }
        s_state.mqtt_was_connected = true;
    }
}

void sp_diag_note_mqtt_connect_attempt(void)
{
    if (sp_lifecycle_ota_reboot_pending()) {
        return;
    }
    saturate_inc(&s_state.mqtt_connect_attempts);
}

void sp_diag_note_mqtt_connect_failure(void)
{
    if (sp_lifecycle_ota_reboot_pending()) {
        return;
    }
    /* Failures while already connected are disconnects, not connect failures. */
    if (!s_state.mqtt_was_connected) {
        saturate_inc(&s_state.mqtt_connect_failures);
    }
}

void sp_diag_note_desired_failure(void)
{
    if (sp_lifecycle_ota_reboot_pending()) {
        return;
    }
    saturate_inc(&s_state.desired_failures);
}

void sp_diag_note_obs_start_rejected(void)
{
    saturate_inc(&s_state.mqtt_observation_start_rejected_count);
}

void sp_diag_note_obs_periodic_rejected(void)
{
    saturate_inc(&s_state.mqtt_observation_periodic_rejected_count);
}

void sp_diag_note_obs_end_rejected(void)
{
    saturate_inc(&s_state.mqtt_observation_end_rejected_count);
}

void sp_diag_note_obs_complete_rejected(void)
{
    saturate_inc(&s_state.mqtt_observation_complete_rejected_count);
}

void sp_diag_note_obs_skipped_disconnected(void)
{
    saturate_inc(&s_state.mqtt_observation_skipped_disconnected_count);
}

static void sp_diag_mem_fill_sample(sp_diag_mem_sample_t *sample)
{
    memset(sample, 0, sizeof(*sample));
    sample->free_heap = (uint32_t)esp_get_free_heap_size();
    sample->min_free_heap = (uint32_t)esp_get_minimum_free_heap_size();
    sample->free_internal = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    sample->min_free_internal =
        (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    sample->largest_internal =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    uint32_t min_hwm = UINT32_MAX;
    bool any = false;
    for (uint32_t i = 0; i < SP_DIAG_TRACKED_TASKS; ++i) {
        if (!s_tracked_tasks[i].used || s_tracked_tasks[i].handle == NULL) {
            continue;
        }
        const uint32_t hwm =
            (uint32_t)uxTaskGetStackHighWaterMark(s_tracked_tasks[i].handle);
        if (hwm < min_hwm) {
            min_hwm = hwm;
        }
        any = true;
    }
    if (any) {
        sample->stack_sampled = true;
        sample->stack_hwm_min_bytes = min_hwm;
    }
}

static cJSON *sp_diag_mem_build_json(const sp_diag_mem_anomaly_t *anom)
{
    cJSON *mem = cJSON_CreateObject();
    if (mem == NULL || anom == NULL) {
        return mem;
    }
    if (anom->kinds & SP_DIAG_MEM_KIND_FREE_HEAP) {
        cJSON_AddNumberToObject(mem, "free_heap", anom->free_heap);
    }
    if (anom->kinds & SP_DIAG_MEM_KIND_MIN_FREE_HEAP) {
        cJSON_AddNumberToObject(mem, "min_free_heap", anom->min_free_heap);
    }
    if (anom->kinds
        & (SP_DIAG_MEM_KIND_FREE_INTERNAL | SP_DIAG_MEM_KIND_FRAGMENTATION)) {
        cJSON_AddNumberToObject(mem, "free_internal", anom->free_internal);
    }
    if (anom->kinds & SP_DIAG_MEM_KIND_MIN_FREE_INTERNAL) {
        cJSON_AddNumberToObject(mem, "min_free_internal", anom->min_free_internal);
    }
    if (anom->kinds
        & (SP_DIAG_MEM_KIND_LARGEST_INTERNAL | SP_DIAG_MEM_KIND_FRAGMENTATION)) {
        cJSON_AddNumberToObject(mem, "largest_internal", anom->largest_internal);
    }
    if (anom->kinds & SP_DIAG_MEM_KIND_STACK_HWM) {
        cJSON_AddNumberToObject(mem, "stack_hwm_min", anom->stack_hwm_min_bytes);
    }
    return mem;
}

void sp_diag_attach_json(cJSON *state_object)
{
    if (state_object == NULL || !s_cfg.enabled) {
        return;
    }

    const bool has_counters =
        s_state.wifi_disconnects > 0 || s_state.mqtt_disconnects > 0 ||
        s_state.mqtt_reconnects > 0 || s_state.mqtt_connect_attempts > 0 ||
        s_state.mqtt_connect_failures > 0 || s_state.desired_failures > 0 ||
        s_state.mqtt_observation_start_rejected_count > 0 ||
        s_state.mqtt_observation_periodic_rejected_count > 0 ||
        s_state.mqtt_observation_end_rejected_count > 0 ||
        s_state.mqtt_observation_complete_rejected_count > 0 ||
        s_state.mqtt_observation_skipped_disconnected_count > 0;
    const bool has_reset = s_state.reset_reason_pending && s_state.last_reset_reason[0] != '\0';

    sp_diag_mem_sample_t sample;
    sp_diag_mem_anomaly_t anom;
    sp_diag_mem_fill_sample(&sample);
    sp_diag_mem_evaluate(&sample, &anom);
    const bool report_mem = sp_diag_mem_should_report(&s_mem_rep, &anom);
    /* Do not keep a stale pending fingerprint across a healthy evaluation. */
    if (!report_mem) {
        s_mem_pending = false;
        s_mem_pending_fp = 0u;
    }

    if (!has_counters && !has_reset && !report_mem) {
        return;
    }

    cJSON *diag = cJSON_CreateObject();
    if (diag == NULL) {
        return;
    }
    if (s_state.wifi_disconnects > 0) {
        cJSON_AddNumberToObject(diag, "wifi_disconnects", s_state.wifi_disconnects);
    }
    if (s_state.mqtt_disconnects > 0) {
        cJSON_AddNumberToObject(diag, "mqtt_disconnects", s_state.mqtt_disconnects);
    }
    if (s_state.mqtt_reconnects > 0) {
        cJSON_AddNumberToObject(diag, "mqtt_reconnects", s_state.mqtt_reconnects);
    }
    if (s_state.mqtt_connect_attempts > 0) {
        cJSON_AddNumberToObject(diag, "mqtt_connect_attempts", s_state.mqtt_connect_attempts);
    }
    if (s_state.mqtt_connect_failures > 0) {
        cJSON_AddNumberToObject(diag, "mqtt_connect_failures", s_state.mqtt_connect_failures);
    }
    if (s_state.desired_failures > 0) {
        cJSON_AddNumberToObject(diag, "desired_failures", s_state.desired_failures);
    }
    if (s_state.mqtt_observation_start_rejected_count > 0) {
        cJSON_AddNumberToObject(
            diag,
            "mqtt_observation_start_rejected_count",
            s_state.mqtt_observation_start_rejected_count
        );
    }
    if (s_state.mqtt_observation_periodic_rejected_count > 0) {
        cJSON_AddNumberToObject(
            diag,
            "mqtt_observation_periodic_rejected_count",
            s_state.mqtt_observation_periodic_rejected_count
        );
    }
    if (s_state.mqtt_observation_end_rejected_count > 0) {
        cJSON_AddNumberToObject(
            diag,
            "mqtt_observation_end_rejected_count",
            s_state.mqtt_observation_end_rejected_count
        );
    }
    if (s_state.mqtt_observation_complete_rejected_count > 0) {
        cJSON_AddNumberToObject(
            diag,
            "mqtt_observation_complete_rejected_count",
            s_state.mqtt_observation_complete_rejected_count
        );
    }
    if (s_state.mqtt_observation_skipped_disconnected_count > 0) {
        cJSON_AddNumberToObject(
            diag,
            "mqtt_observation_skipped_disconnected_count",
            s_state.mqtt_observation_skipped_disconnected_count
        );
    }
    if (has_reset) {
        cJSON_AddStringToObject(diag, "last_reset_reason", s_state.last_reset_reason);
    }
    if (report_mem) {
        cJSON *mem = sp_diag_mem_build_json(&anom);
        if (mem != NULL) {
            cJSON_AddItemToObject(diag, "mem", mem);
            s_mem_pending = true;
            s_mem_pending_fp = anom.fingerprint;
            ESP_LOGW(
                TAG_MEM,
                "anomaly kinds=0x%lx free_heap=%lu free_internal=%lu "
                "largest_internal=%lu stack_hwm_min=%lu",
                (unsigned long)anom.kinds,
                (unsigned long)anom.free_heap,
                (unsigned long)anom.free_internal,
                (unsigned long)anom.largest_internal,
                (unsigned long)anom.stack_hwm_min_bytes
            );
        }
    }
    cJSON_AddItemToObject(state_object, "diag", diag);
}

void sp_diag_clear_oneshots(void)
{
    s_state.reset_reason_pending = false;
    if (s_mem_pending) {
        sp_diag_mem_note_published(&s_mem_rep, s_mem_pending_fp);
        s_mem_pending = false;
        s_mem_pending_fp = 0u;
    }
}

void sp_diag_register_task(const char *name, void *task_handle)
{
    if (name == NULL || name[0] == '\0') {
        return;
    }
    TaskHandle_t handle = (TaskHandle_t)task_handle;
    for (uint32_t i = 0; i < SP_DIAG_TRACKED_TASKS; ++i) {
        if (s_tracked_tasks[i].used
            && strncmp(s_tracked_tasks[i].name, name, sizeof(s_tracked_tasks[i].name))
                == 0) {
            s_tracked_tasks[i].handle = handle;
            if (handle == NULL) {
                s_tracked_tasks[i].used = false;
                s_tracked_tasks[i].name[0] = '\0';
            }
            return;
        }
    }
    if (handle == NULL) {
        return;
    }
    for (uint32_t i = 0; i < SP_DIAG_TRACKED_TASKS; ++i) {
        if (!s_tracked_tasks[i].used) {
            strncpy(
                s_tracked_tasks[i].name,
                name,
                sizeof(s_tracked_tasks[i].name) - 1u
            );
            s_tracked_tasks[i].name[sizeof(s_tracked_tasks[i].name) - 1u] = '\0';
            s_tracked_tasks[i].handle = handle;
            s_tracked_tasks[i].used = true;
            return;
        }
    }
}

void sp_diag_log_resources(const char *ctx)
{
    const int cpu_hz = esp_clk_cpu_freq();
    const unsigned long cpu_mhz =
        cpu_hz > 0 ? (unsigned long)(cpu_hz / 1000000) : 0ul;
#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
    const char *pm = "enabled";
#else
    const char *pm = "disabled";
#endif
    const unsigned long hwm_bytes =
        (unsigned long)uxTaskGetStackHighWaterMark(NULL);
    const unsigned long free_heap = (unsigned long)esp_get_free_heap_size();
    const unsigned long min_free_heap =
        (unsigned long)esp_get_minimum_free_heap_size();
    const unsigned long free_internal =
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const unsigned long min_free_internal =
        (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const unsigned long largest_internal =
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const unsigned long free_spiram =
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const unsigned long min_free_spiram =
        (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    const unsigned long largest_spiram =
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    ESP_LOGI(
        TAG_SP,
        "res ctx=%s cpu_mhz=%lu configured_mhz=%d tick_hz=%d pm=%s "
        "free_heap=%lu min_free_heap=%lu "
        "free_internal=%lu min_free_internal=%lu largest_internal=%lu "
        "free_spiram=%lu min_free_spiram=%lu largest_spiram=%lu "
        "stack_hwm_bytes=%lu",
        (ctx != NULL && ctx[0] != '\0') ? ctx : "-",
        cpu_mhz,
        CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        CONFIG_FREERTOS_HZ,
        pm,
        free_heap,
        min_free_heap,
        free_internal,
        min_free_internal,
        largest_internal,
        free_spiram,
        min_free_spiram,
        largest_spiram,
        hwm_bytes
    );

    for (uint32_t i = 0; i < SP_DIAG_TRACKED_TASKS; ++i) {
        if (!s_tracked_tasks[i].used || s_tracked_tasks[i].handle == NULL) {
            continue;
        }
        const unsigned long thwm =
            (unsigned long)uxTaskGetStackHighWaterMark(s_tracked_tasks[i].handle);
        ESP_LOGI(
            TAG_SP,
            "res ctx=%s task=%s stack_hwm_bytes=%lu",
            (ctx != NULL && ctx[0] != '\0') ? ctx : "-",
            s_tracked_tasks[i].name,
            thwm
        );
    }
}
