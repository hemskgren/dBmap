#include "sp_diag.h"

#include <string.h>

#include "sp_lifecycle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG_SP = "sp";

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
    if (!has_counters && !has_reset) {
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
    cJSON_AddItemToObject(state_object, "diag", diag);
}

void sp_diag_clear_oneshots(void)
{
    s_state.reset_reason_pending = false;
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
    ESP_LOGI(
        TAG_SP,
        "res ctx=%s cpu_mhz=%lu configured_mhz=%d pm=%s free_heap=%lu "
        "min_free_heap=%lu free_spiram=%lu stack_hwm_bytes=%lu",
        (ctx != NULL && ctx[0] != '\0') ? ctx : "-",
        cpu_mhz,
        CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        pm,
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        hwm_bytes
    );
}
