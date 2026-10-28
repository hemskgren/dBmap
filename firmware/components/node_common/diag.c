#include "dbmap_diag.h"

#include <string.h>

#include "dbmap_lifecycle.h"
#include "esp_system.h"
#include "sdkconfig.h"

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
    char last_reset_reason[24];
    bool reset_reason_pending;
    bool mqtt_was_connected;
} dbmap_diag_state_t;

static dbmap_diag_config_t s_cfg;
static dbmap_diag_state_t s_state;

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

void dbmap_diag_config_defaults(dbmap_diag_config_t *out)
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

void dbmap_diag_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    dbmap_diag_config_defaults(&s_cfg);
    const char *reason = reset_reason_name(esp_reset_reason());
    strncpy(s_state.last_reset_reason, reason, sizeof(s_state.last_reset_reason) - 1);
    s_state.reset_reason_pending = true;
}

void dbmap_diag_apply_desired_payload(const cJSON *payload)
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

const dbmap_diag_config_t *dbmap_diag_config(void)
{
    return &s_cfg;
}

void dbmap_diag_note_wifi_disconnect(void)
{
    if (dbmap_lifecycle_ota_reboot_pending()) {
        return;
    }
    saturate_inc(&s_state.wifi_disconnects);
}

void dbmap_diag_note_mqtt_disconnect(void)
{
    if (dbmap_lifecycle_ota_reboot_pending()) {
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

void dbmap_diag_note_mqtt_reconnect(void)
{
    if (dbmap_lifecycle_ota_reboot_pending()) {
        return;
    }
    if (!s_state.mqtt_was_connected) {
        if (s_state.mqtt_disconnects > 0) {
            saturate_inc(&s_state.mqtt_reconnects);
        }
        s_state.mqtt_was_connected = true;
    }
}

void dbmap_diag_note_mqtt_connect_attempt(void)
{
    if (dbmap_lifecycle_ota_reboot_pending()) {
        return;
    }
    saturate_inc(&s_state.mqtt_connect_attempts);
}

void dbmap_diag_note_mqtt_connect_failure(void)
{
    if (dbmap_lifecycle_ota_reboot_pending()) {
        return;
    }
    /* Failures while already connected are disconnects, not connect failures. */
    if (!s_state.mqtt_was_connected) {
        saturate_inc(&s_state.mqtt_connect_failures);
    }
}

void dbmap_diag_note_desired_failure(void)
{
    if (dbmap_lifecycle_ota_reboot_pending()) {
        return;
    }
    saturate_inc(&s_state.desired_failures);
}

void dbmap_diag_attach_json(cJSON *state_object)
{
    if (state_object == NULL || !s_cfg.enabled) {
        return;
    }

    const bool has_counters =
        s_state.wifi_disconnects > 0 || s_state.mqtt_disconnects > 0 ||
        s_state.mqtt_reconnects > 0 || s_state.mqtt_connect_attempts > 0 ||
        s_state.mqtt_connect_failures > 0 || s_state.desired_failures > 0;
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
    if (has_reset) {
        cJSON_AddStringToObject(diag, "last_reset_reason", s_state.last_reset_reason);
    }
    cJSON_AddItemToObject(state_object, "diag", diag);
}

void dbmap_diag_clear_oneshots(void)
{
    s_state.reset_reason_pending = false;
}
