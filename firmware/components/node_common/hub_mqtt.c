#include "dbmap_mqtt.h"

#include <stdio.h>
#include <string.h>

#include "dbmap_backoff.h"
#include "dbmap_diag.h"
#include "dbmap_lifecycle.h"
#include "dbmap_resolve.h"
#include "dbmap_wifi.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

extern const uint8_t root_ca_pem_start[] asm("_binary_root_ca_pem_start");

static const char *TAG = "dbmap_mqtt";
static esp_mqtt_client_handle_t s_client;
static dbmap_command_cb_t s_cmd;
static void *s_ctx;
static const dbmap_effective_t *s_cfg;
static bool s_connected;
static bool s_disconnect_handled;
static char s_uri[192];
static char s_client_id[DBMAP_NODE_ID_MAX];
static dbmap_backoff_t s_backoff;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_stable_timer;
static EventGroupHandle_t s_pub_events;
static int s_wait_msg_id;
#define PUBACK_BIT BIT0

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint32_t mqtt_rand32(void)
{
    return esp_random();
}

static void reconnect_timer_cb(void *arg);
static void stable_timer_cb(void *arg);
static void schedule_reconnect(void);

static void cancel_timer(esp_timer_handle_t *timer)
{
    if (timer == NULL || *timer == NULL) {
        return;
    }
    esp_timer_stop(*timer);
    esp_timer_delete(*timer);
    *timer = NULL;
}

static void arm_oneshot(esp_timer_handle_t *timer, uint64_t delay_ms, esp_timer_cb_t cb)
{
    cancel_timer(timer);
    const esp_timer_create_args_t args = {
        .callback = cb,
        .name = "mqtt_bo",
    };
    if (esp_timer_create(&args, timer) != ESP_OK) {
        return;
    }
    if (delay_ms == 0) {
        delay_ms = 1;
    }
    if (esp_timer_start_once(*timer, delay_ms * 1000ULL) != ESP_OK) {
        cancel_timer(timer);
    }
}

static void try_reconnect(void)
{
    if (dbmap_lifecycle_ota_reboot_pending()) {
        return;
    }
    if (s_client == NULL || s_connected) {
        return;
    }
    if (dbmap_backoff_is_in_flight(&s_backoff)) {
        return;
    }
    if (!dbmap_wifi_has_ip()) {
        arm_oneshot(&s_reconnect_timer, 2000, reconnect_timer_cb);
        return;
    }
    if (!dbmap_backoff_ready(&s_backoff, now_ms())) {
        schedule_reconnect();
        return;
    }

    dbmap_backoff_set_in_flight(&s_backoff, true);
    dbmap_diag_note_mqtt_connect_attempt();
    esp_err_t err = esp_mqtt_client_reconnect(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reconnect start failed: %s", esp_err_to_name(err));
        dbmap_diag_note_mqtt_connect_failure();
        dbmap_backoff_note_failure(&s_backoff, now_ms());
        schedule_reconnect();
    }
}

static void schedule_reconnect(void)
{
    if (dbmap_lifecycle_ota_reboot_pending()) {
        return;
    }
    uint32_t delay = dbmap_backoff_delay_ms(&s_backoff, mqtt_rand32);
    int64_t ready_in = s_backoff.next_attempt_at_ms - now_ms();
    if (ready_in > 0 && (uint32_t)ready_in < delay) {
        delay = (uint32_t)ready_in;
    }
    if (delay < 1) {
        delay = 1;
    }
    ESP_LOGI(TAG, "scheduling reconnect in %lu ms (failures=%lu)",
             (unsigned long)delay,
             (unsigned long)s_backoff.consecutive_failures);
    arm_oneshot(&s_reconnect_timer, delay, reconnect_timer_cb);
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    try_reconnect();
}

static void stable_timer_cb(void *arg)
{
    (void)arg;
    if (dbmap_backoff_poll_stable(&s_backoff, now_ms())) {
        ESP_LOGI(TAG, "MQTT connection stable; backoff reset");
    }
}

static void on_connected(void)
{
    s_connected = true;
    s_disconnect_handled = false;
    dbmap_backoff_set_in_flight(&s_backoff, false);
    cancel_timer(&s_reconnect_timer);
    if (dbmap_lifecycle_ota_reboot_pending()) {
        ESP_LOGI(TAG, "connected during OTA reboot pending (ignored)");
        return;
    }
    dbmap_diag_note_mqtt_reconnect();
    dbmap_backoff_note_success(&s_backoff, now_ms());
    if (s_backoff.cfg.stable_reset_ms > 0) {
        arm_oneshot(&s_stable_timer, s_backoff.cfg.stable_reset_ms, stable_timer_cb);
    }
    ESP_LOGI(TAG, "connected");
    if (s_cfg) {
        esp_mqtt_client_subscribe(s_client, s_cfg->topics.command, 1);
    }
}

static void on_disconnected(void)
{
    const bool was_connected = s_connected;
    s_connected = false;
    dbmap_backoff_set_in_flight(&s_backoff, false);
    cancel_timer(&s_stable_timer);

    if (dbmap_lifecycle_ota_reboot_pending()) {
        /* Expected teardown before OTA reboot: log only, no reconnect/diag. */
        ESP_LOGI(TAG, "disconnect during OTA reboot pending (expected)");
        s_disconnect_handled = true;
        (void)was_connected;
        dbmap_diag_note_mqtt_disconnect();
        return;
    }

    if (s_disconnect_handled) {
        /* Duplicate DISCONNECTED while already down — do not re-count or steepen. */
        if (s_reconnect_timer == NULL) {
            schedule_reconnect();
        }
        return;
    }
    s_disconnect_handled = true;

    if (was_connected) {
        dbmap_diag_note_mqtt_disconnect();
    } else {
        dbmap_diag_note_mqtt_connect_failure();
    }
    dbmap_backoff_note_failure(&s_backoff, now_ms());
    schedule_reconnect();
}

static void mqtt_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        on_connected();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        on_disconnected();
        break;
    case MQTT_EVENT_ERROR:
        /* Log only. DISCONNECTED is the authoritative transition for counters/backoff. */
        if (event->error_handle) {
            ESP_LOGW(TAG,
                     "error type=%d connect_return=%d esp_tls=%d tls_stack=%d sock=%d",
                     (int)event->error_handle->error_type,
                     (int)event->error_handle->connect_return_code,
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_tls_stack_err,
                     event->error_handle->esp_tls_cert_verify_flags);
        } else {
            ESP_LOGW(TAG, "error");
        }
        break;
    case MQTT_EVENT_DATA:
        if (s_cmd && s_cfg && event->topic_len == (int)strlen(s_cfg->topics.command) &&
            memcmp(event->topic, s_cfg->topics.command, event->topic_len) == 0) {
            s_cmd(event->data, event->data_len, s_ctx);
        }
        break;
    case MQTT_EVENT_PUBLISHED:
        if (s_pub_events != NULL && event->msg_id == s_wait_msg_id && s_wait_msg_id > 0) {
            xEventGroupSetBits(s_pub_events, PUBACK_BIT);
        }
        break;
    default:
        break;
    }
}

esp_err_t dbmap_mqtt_start(const dbmap_effective_t *cfg, dbmap_command_cb_t on_command, void *ctx)
{
    s_cfg = cfg;
    s_cmd = on_command;
    s_ctx = ctx;
    s_connected = false;
    s_disconnect_handled = false;
    s_wait_msg_id = 0;
    if (s_pub_events == NULL) {
        s_pub_events = xEventGroupCreate();
    }
    dbmap_backoff_config_t bo_cfg;
    dbmap_backoff_config_mqtt(&bo_cfg);
    dbmap_backoff_init(&s_backoff, &bo_cfg);

    const char *host = cfg->network.mqtt_host[0] ? cfg->network.mqtt_host : "hub.local";
    if (!cfg->network.mqtt_use_tls || cfg->network.mqtt_username[0] == '\0' ||
        cfg->network.mqtt_password[0] == '\0') {
        ESP_LOGE(TAG, "refusing MQTT connection without TLS and per-node credentials");
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg->identity.node_id[0] == '\0') {
        ESP_LOGE(TAG, "refusing MQTT connection without a node id for the client id");
        return ESP_ERR_INVALID_STATE;
    }

    char resolved_host[16];
    if (dbmap_resolve_ipv4(host, resolved_host, sizeof(resolved_host)) == ESP_OK) {
        ESP_LOGI(TAG, "MQTT host %s -> %s", host, resolved_host);
        host = resolved_host;
    } else {
        ESP_LOGW(TAG, "could not pre-resolve MQTT host %s; broker client will retry", host);
    }
    int uri_len = snprintf(s_uri, sizeof(s_uri), "mqtts://%s:%u", host, cfg->network.mqtt_port);
    if (uri_len <= 0 || (size_t)uri_len >= sizeof(s_uri)) {
        ESP_LOGE(TAG, "MQTT URI is too long");
        return ESP_ERR_INVALID_SIZE;
    }
    strncpy(s_client_id, cfg->identity.node_id, sizeof(s_client_id) - 1);
    s_client_id[sizeof(s_client_id) - 1] = '\0';

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_uri,
        .broker.verification.certificate = (const char *)root_ca_pem_start,
        .credentials.client_id = s_client_id,
        .credentials.username = cfg->network.mqtt_username,
        .credentials.authentication.password = cfg->network.mqtt_password,
        .session.keepalive = 60,
        .network.timeout_ms = 20000,
        .network.disable_auto_reconnect = true,
        .buffer.size = 2048,
    };
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    ESP_LOGI(TAG, "starting client_id=%s uri=%s (exponential reconnect backoff)", s_client_id, s_uri);
    dbmap_backoff_set_in_flight(&s_backoff, true);
    dbmap_diag_note_mqtt_connect_attempt();
    return esp_mqtt_client_start(s_client);
}

esp_mqtt_client_handle_t dbmap_mqtt_handle(void)
{
    return s_client;
}

bool dbmap_mqtt_is_connected(void)
{
    return s_connected && s_client != NULL;
}

esp_err_t dbmap_mqtt_publish_json(const char *topic, const char *json, int qos)
{
    if (!s_client) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, json, 0, qos, 0);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t dbmap_mqtt_publish_json_wait(
    const char *topic,
    const char *json,
    int qos,
    uint32_t timeout_ms
)
{
    if (!s_client || !s_connected || s_pub_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms == 0) {
        timeout_ms = 1;
    }
    xEventGroupClearBits(s_pub_events, PUBACK_BIT);
    int msg_id = esp_mqtt_client_publish(s_client, topic, json, 0, qos, 0);
    if (msg_id < 0) {
        return ESP_FAIL;
    }
    if (qos <= 0) {
        /* QoS 0 has no PUBACK; treat enqueue as success. */
        return ESP_OK;
    }
    s_wait_msg_id = msg_id;
    EventBits_t bits = xEventGroupWaitBits(
        s_pub_events,
        PUBACK_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms)
    );
    s_wait_msg_id = 0;
    return (bits & PUBACK_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void dbmap_mqtt_prepare_ota_reboot(void)
{
    cancel_timer(&s_reconnect_timer);
    cancel_timer(&s_stable_timer);
    dbmap_backoff_set_in_flight(&s_backoff, false);
    if (s_client == NULL || !s_connected) {
        return;
    }
    /* Best-effort quick disconnect; DISCONNECTED is treated as expected teardown. */
    esp_err_t err = esp_mqtt_client_disconnect(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA reboot disconnect request failed: %s", esp_err_to_name(err));
    }
}
