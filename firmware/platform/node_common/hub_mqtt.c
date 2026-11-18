#include "sp_mqtt.h"

#include <stdio.h>
#include <strings.h>
#include <string.h>

#include "sp_backoff.h"
#include "sp_connectivity.h"
#include "sp_diag.h"
#include "sp_lifecycle.h"
#include "sp_mqtt_progress.h"
#include "sp_net_dispatch.h"
#include "sp_resolve.h"
#include "sp_wifi.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

extern const uint8_t root_ca_pem_start[] asm("_binary_root_ca_pem_start");

static const char *TAG = "sp_mqtt";
static esp_mqtt_client_handle_t s_client;
static sp_command_cb_t s_cmd;
static void *s_ctx;
static const sp_effective_t *s_cfg;
static bool s_connected;
static bool s_disconnect_handled;
static char s_uri[192];
static char s_client_id[DBMAP_NODE_ID_MAX];
static sp_backoff_t s_backoff;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_stable_timer;
static esp_timer_handle_t s_connect_watchdog;
static esp_timer_handle_t s_progress_timer;
static sp_mqtt_progress_t s_progress;
static SemaphoreHandle_t s_progress_mu;
static char s_configured_host[DBMAP_HOST_MAX];
static char s_verify_name[DBMAP_HOST_MAX];
static uint16_t s_mqtt_port;
static bool s_recreate_active;
static EventGroupHandle_t s_pub_events;
static int s_wait_msg_id;
static bool s_force_stop_start;
#define PUBACK_BIT BIT0
/* Longer than network.timeout_ms so a hanging connect cannot leave in_flight stuck. */
#define MQTT_CONNECT_WATCHDOG_MS 30000u
#define MQTT_RECONNECT_ESCALATE_AFTER 3u

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void progress_lock(void)
{
    if (s_progress_mu != NULL) {
        (void)xSemaphoreTake(s_progress_mu, portMAX_DELAY);
    }
}

static void progress_unlock(void)
{
    if (s_progress_mu != NULL) {
        (void)xSemaphoreGive(s_progress_mu);
    }
}

static int64_t hook_progress_age(int64_t now)
{
    int64_t age;
    progress_lock();
    age = sp_mqtt_progress_age_ms(&s_progress, now);
    progress_unlock();
    return age;
}

static bool hook_progress_expired(int64_t now)
{
    bool expired;
    progress_lock();
    expired = sp_mqtt_progress_expired(&s_progress, now);
    progress_unlock();
    return expired;
}

static int64_t hook_next_attempt_in(int64_t now)
{
    if (s_connected) {
        return 0;
    }
    if (s_backoff.next_attempt_at_ms <= now) {
        return 0;
    }
    return s_backoff.next_attempt_at_ms - now;
}

static int64_t hook_last_verified(void)
{
    int64_t stamp;
    progress_lock();
    stamp = sp_mqtt_progress_last_verified_ms(&s_progress);
    progress_unlock();
    return stamp;
}

static uint32_t mqtt_rand32(void)
{
    return esp_random();
}

static void reconnect_timer_cb(void *arg);
static void stable_timer_cb(void *arg);
static void connect_watchdog_cb(void *arg);
static void progress_timer_cb(void *arg);
static void schedule_reconnect(void);
static void arm_connect_watchdog(void);
static void arm_progress_timer(void);
static esp_err_t mqtt_recreate(void);
static void mqtt_on_connect_watchdog(void);
static void mqtt_on_progress_expired(void);
static void mqtt_event(
    void *handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data
);

static void cancel_timer(esp_timer_handle_t *timer)
{
    if (timer == NULL || *timer == NULL) {
        return;
    }
    esp_timer_stop(*timer);
    esp_timer_delete(*timer);
    *timer = NULL;
}

static bool arm_oneshot(esp_timer_handle_t *timer, uint64_t delay_ms, esp_timer_cb_t cb)
{
    cancel_timer(timer);
    const esp_timer_create_args_t args = {
        .callback = cb,
        .name = "mqtt_bo",
    };
    if (esp_timer_create(&args, timer) != ESP_OK) {
        *timer = NULL;
        return false;
    }
    if (delay_ms == 0) {
        delay_ms = 1;
    }
    if (esp_timer_start_once(*timer, delay_ms * 1000ULL) != ESP_OK) {
        cancel_timer(timer);
        return false;
    }
    return true;
}

static void defer_mqtt_op(sp_net_op_t op, uint32_t delay_ms)
{
    if (delay_ms < SP_CONN_TIMER_RETRY_MIN_MS) {
        delay_ms = SP_CONN_TIMER_RETRY_MIN_MS;
    }
    sp_net_dispatch_post_after(op, delay_ms);
}

static void arm_connect_watchdog(void)
{
    if (!arm_oneshot(
            &s_connect_watchdog, MQTT_CONNECT_WATCHDOG_MS, connect_watchdog_cb
        )) {
        ESP_LOGW(TAG, "failed to arm MQTT connect watchdog; deferring");
        sp_connectivity_note_timer_arm_failed(SP_CONN_FAIL_MQTT_WATCHDOG_TIMER);
        defer_mqtt_op(SP_NET_OP_MQTT_WATCHDOG, SP_CONN_TIMER_RETRY_MIN_MS);
        return;
    }
    sp_connectivity_note_timer_arm_ok();
}

static void arm_progress_timer(void)
{
    if (!arm_oneshot(
            &s_progress_timer, SP_MQTT_PROGRESS_LIMIT_MS, progress_timer_cb
        )) {
        ESP_LOGW(TAG, "failed to arm MQTT progress timer; deferring");
        sp_connectivity_note_timer_arm_failed(SP_CONN_FAIL_MQTT_PROGRESS_TIMER);
        defer_mqtt_op(SP_NET_OP_MQTT_PROGRESS, SP_CONN_TIMER_RETRY_MIN_MS);
        return;
    }
    sp_connectivity_note_timer_arm_ok();
}

static bool host_is_local(const char *host)
{
    size_t len;
    if (host == NULL) {
        return false;
    }
    len = strlen(host);
    return len > 6 && strcasecmp(host + len - 6, ".local") == 0;
}

/** Refresh s_uri. May block in DNS/mDNS. Call only from the recovery task or boot. */
static esp_err_t rebuild_broker_uri(void)
{
    const char *connect_host = s_configured_host[0] ? s_configured_host : "hub.local";
    char resolved_host[16];
    s_verify_name[0] = '\0';
    if (host_is_local(connect_host)) {
        if (sp_resolve_ipv4(connect_host, resolved_host, sizeof(resolved_host)) == ESP_OK) {
            ESP_LOGI(TAG, "MQTT host %s -> %s", connect_host, resolved_host);
            connect_host = resolved_host;
            strncpy(s_verify_name, s_configured_host, sizeof(s_verify_name) - 1);
            s_verify_name[sizeof(s_verify_name) - 1] = '\0';
        } else {
            ESP_LOGW(TAG, "could not pre-resolve MQTT host %s; keeping DNS name",
                     s_configured_host);
        }
    } else if (sp_resolve_ipv4(connect_host, resolved_host, sizeof(resolved_host)) == ESP_OK) {
        ESP_LOGI(TAG, "MQTT host %s resolves to %s (keeping DNS name for TLS)",
                 connect_host, resolved_host);
    } else {
        ESP_LOGW(TAG, "could not pre-resolve MQTT host %s; keeping DNS name", connect_host);
    }
    int uri_len = snprintf(
        s_uri, sizeof(s_uri), "mqtts://%s:%u", connect_host, (unsigned)s_mqtt_port
    );
    if (uri_len <= 0 || (size_t)uri_len >= sizeof(s_uri)) {
        s_uri[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void try_reconnect(void)
{
    if (!sp_net_dispatch_is_worker()) {
        sp_net_dispatch_post(SP_NET_OP_MQTT_RECONNECT);
        return;
    }
    if (sp_lifecycle_ota_reboot_pending()) {
        return;
    }
    if (s_client == NULL || s_connected) {
        return;
    }
    if (sp_backoff_is_in_flight(&s_backoff)) {
        /* A prior reconnect() returned OK but CONNECTED/DISCONNECTED never arrived.
         * Keep a watchdog alive so in_flight cannot stall reconnect forever. */
        if (s_connect_watchdog == NULL) {
            ESP_LOGW(TAG, "connect still in flight; arming watchdog");
            arm_connect_watchdog();
        }
        return;
    }
    if (!sp_wifi_has_ip()) {
        ESP_LOGW(TAG, "deferring MQTT reconnect; Wi-Fi has no IP");
        if (!arm_oneshot(&s_reconnect_timer, 2000, reconnect_timer_cb)) {
            sp_connectivity_note_timer_arm_failed(SP_CONN_FAIL_MQTT_RECONNECT_TIMER);
            defer_mqtt_op(SP_NET_OP_MQTT_RECONNECT, 2000);
        }
        return;
    }
    if (!sp_backoff_ready(&s_backoff, now_ms())) {
        schedule_reconnect();
        return;
    }

    const bool escalate = s_force_stop_start ||
                          s_backoff.consecutive_failures >= MQTT_RECONNECT_ESCALATE_AFTER;
    s_force_stop_start = false;
    esp_err_t err;
    if (escalate) {
        ESP_LOGW(TAG, "MQTT recovery via client recreate (failures=%lu)",
                 (unsigned long)s_backoff.consecutive_failures);
        err = mqtt_recreate();
    } else {
        sp_backoff_set_in_flight(&s_backoff, true);
        sp_diag_note_mqtt_connect_attempt();
        arm_connect_watchdog();
        err = esp_mqtt_client_reconnect(s_client);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reconnect start failed: %s", esp_err_to_name(err));
        cancel_timer(&s_connect_watchdog);
        sp_diag_note_mqtt_connect_failure();
        sp_backoff_note_failure(&s_backoff, now_ms());
        sp_backoff_set_in_flight(&s_backoff, false);
        s_force_stop_start = true;
        schedule_reconnect();
    }
}

static esp_err_t mqtt_open_client(void)
{
    if (s_cfg == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_uri,
        .broker.verification.certificate = (const char *)root_ca_pem_start,
        .broker.verification.common_name = s_verify_name[0] ? s_verify_name : NULL,
        .credentials.client_id = s_client_id,
        .credentials.username = s_cfg->network.mqtt_username,
        .credentials.authentication.password = s_cfg->network.mqtt_password,
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
    return ESP_OK;
}

static esp_err_t mqtt_recreate(void)
{
    esp_err_t uri_err = rebuild_broker_uri();
    if (uri_err != ESP_OK) {
        return uri_err;
    }
    s_recreate_active = true;
    s_disconnect_handled = true;
    s_connected = false;
    sp_connectivity_note_mqtt_connected(false);
    cancel_timer(&s_reconnect_timer);
    cancel_timer(&s_stable_timer);
    cancel_timer(&s_connect_watchdog);
    cancel_timer(&s_progress_timer);
    progress_lock();
    sp_mqtt_progress_on_disconnected(&s_progress);
    progress_unlock();
    if (s_client != NULL) {
        (void)esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    esp_err_t err = mqtt_open_client();
    if (err != ESP_OK) {
        s_recreate_active = false;
        return err;
    }
    sp_backoff_set_in_flight(&s_backoff, true);
    sp_diag_note_mqtt_connect_attempt();
    arm_connect_watchdog();
    err = esp_mqtt_client_start(s_client);
    s_recreate_active = false;
    return err;
}

static void mqtt_worker_reconnect(void)
{
    try_reconnect();
}

static void mqtt_worker_watchdog(void)
{
    mqtt_on_connect_watchdog();
}

static void mqtt_worker_progress(void)
{
    mqtt_on_progress_expired();
}

static void schedule_reconnect(void)
{
    if (sp_lifecycle_ota_reboot_pending()) {
        return;
    }
    uint32_t delay = sp_backoff_delay_ms(&s_backoff, mqtt_rand32);
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
    if (!arm_oneshot(&s_reconnect_timer, delay, reconnect_timer_cb)) {
        ESP_LOGW(TAG, "failed to arm MQTT reconnect timer; deferring via sp_net");
        sp_connectivity_note_timer_arm_failed(SP_CONN_FAIL_MQTT_RECONNECT_TIMER);
        defer_mqtt_op(SP_NET_OP_MQTT_RECONNECT, delay);
        return;
    }
    sp_connectivity_note_timer_arm_ok();
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    sp_net_dispatch_post(SP_NET_OP_MQTT_RECONNECT);
}

static void connect_watchdog_cb(void *arg)
{
    (void)arg;
    sp_net_dispatch_post(SP_NET_OP_MQTT_WATCHDOG);
}

static void progress_timer_cb(void *arg)
{
    (void)arg;
    sp_net_dispatch_post(SP_NET_OP_MQTT_PROGRESS);
}

static void mqtt_on_connect_watchdog(void)
{
    if (s_connected || sp_lifecycle_ota_reboot_pending()) {
        return;
    }
    ESP_LOGW(TAG, "MQTT connect watchdog fired; forcing client recreate");
    cancel_timer(&s_reconnect_timer);
    sp_backoff_set_in_flight(&s_backoff, false);
    sp_diag_note_mqtt_connect_failure();
    sp_backoff_note_failure(&s_backoff, now_ms());
    s_disconnect_handled = true;
    s_force_stop_start = true;
    schedule_reconnect();
}

static void mqtt_on_progress_expired(void)
{
    bool expired;
    if (sp_lifecycle_ota_reboot_pending()) {
        return;
    }
    progress_lock();
    expired = sp_mqtt_progress_expired(&s_progress, now_ms());
    progress_unlock();
    if (!s_connected) {
        /* Timer-arm retry path may land here while already clearing. */
        return;
    }
    if (!expired) {
        /* Deferred re-arm after a timer create failure, or supervisor nudge. */
        arm_progress_timer();
        return;
    }
    ESP_LOGW(
        TAG,
        "no verified MQTT progress for %lu ms; recreating client",
        (unsigned long)SP_MQTT_PROGRESS_LIMIT_MS
    );
    s_connected = false;
    sp_connectivity_note_mqtt_connected(false);
    s_disconnect_handled = true;
    s_force_stop_start = true;
    sp_diag_note_mqtt_disconnect();
    sp_backoff_set_in_flight(&s_backoff, false);
    sp_backoff_note_failure(&s_backoff, now_ms());
    esp_err_t err = mqtt_recreate();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "progress recovery recreate failed: %s", esp_err_to_name(err));
        cancel_timer(&s_connect_watchdog);
        sp_diag_note_mqtt_connect_failure();
        sp_backoff_note_failure(&s_backoff, now_ms());
        sp_backoff_set_in_flight(&s_backoff, false);
        s_force_stop_start = true;
        schedule_reconnect();
    }
}

static void stable_timer_cb(void *arg)
{
    (void)arg;
    if (sp_backoff_poll_stable(&s_backoff, now_ms())) {
        ESP_LOGI(TAG, "MQTT connection stable; backoff reset");
    }
}

static void on_connected(void)
{
    s_connected = true;
    sp_connectivity_note_mqtt_connected(true);
    s_disconnect_handled = false;
    s_force_stop_start = false;
    sp_backoff_set_in_flight(&s_backoff, false);
    cancel_timer(&s_reconnect_timer);
    cancel_timer(&s_connect_watchdog);
    if (sp_lifecycle_ota_reboot_pending()) {
        ESP_LOGI(TAG, "connected during OTA reboot pending (ignored)");
        return;
    }
    sp_diag_note_mqtt_reconnect();
    sp_backoff_note_success(&s_backoff, now_ms());
    progress_lock();
    sp_mqtt_progress_on_connected(&s_progress, now_ms());
    progress_unlock();
    arm_progress_timer();
    if (s_backoff.cfg.stable_reset_ms > 0) {
        if (!arm_oneshot(
                &s_stable_timer, s_backoff.cfg.stable_reset_ms, stable_timer_cb
            )) {
            ESP_LOGW(TAG, "failed to arm MQTT stable timer");
            sp_connectivity_note_timer_arm_failed(SP_CONN_FAIL_MQTT_STABLE_TIMER);
        }
    }
    ESP_LOGI(TAG, "connected");
    if (s_cfg) {
        esp_mqtt_client_subscribe(s_client, s_cfg->topics.command, 1);
    }
}

static void on_disconnected(void)
{
    if (s_recreate_active) {
        s_connected = false;
        sp_connectivity_note_mqtt_connected(false);
        sp_backoff_set_in_flight(&s_backoff, false);
        progress_lock();
        sp_mqtt_progress_on_disconnected(&s_progress);
        progress_unlock();
        cancel_timer(&s_stable_timer);
        cancel_timer(&s_progress_timer);
        return;
    }
    const bool was_connected = s_connected;
    s_connected = false;
    sp_connectivity_note_mqtt_connected(false);
    sp_backoff_set_in_flight(&s_backoff, false);
    progress_lock();
    sp_mqtt_progress_on_disconnected(&s_progress);
    progress_unlock();
    cancel_timer(&s_stable_timer);
    cancel_timer(&s_connect_watchdog);
    cancel_timer(&s_progress_timer);

    if (sp_lifecycle_ota_reboot_pending()) {
        /* Expected teardown before OTA reboot: log only, no reconnect/diag. */
        ESP_LOGI(TAG, "disconnect during OTA reboot pending (expected)");
        s_disconnect_handled = true;
        (void)was_connected;
        sp_diag_note_mqtt_disconnect();
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
        sp_diag_note_mqtt_disconnect();
    } else {
        sp_diag_note_mqtt_connect_failure();
    }
    sp_backoff_note_failure(&s_backoff, now_ms());
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
        /* Prefer DISCONNECTED for counters; still keep recovery alive if ERROR is alone. */
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
        if (!s_connected && !sp_lifecycle_ota_reboot_pending()) {
            if (sp_backoff_is_in_flight(&s_backoff) && s_connect_watchdog == NULL) {
                arm_connect_watchdog();
            } else if (s_reconnect_timer == NULL && !sp_backoff_is_in_flight(&s_backoff)) {
                schedule_reconnect();
            }
        }
        break;
    case MQTT_EVENT_DATA: {
        bool rearm = false;
        progress_lock();
        sp_mqtt_progress_on_inbound_data(&s_progress, now_ms());
        rearm = s_progress.connected;
        progress_unlock();
        if (rearm) {
            arm_progress_timer();
            sp_connectivity_note_mqtt_verified();
        }
        if (s_cmd && s_cfg && event->topic_len == (int)strlen(s_cfg->topics.command) &&
            memcmp(event->topic, s_cfg->topics.command, event->topic_len) == 0) {
            s_cmd(event->data, event->data_len, s_ctx);
        }
        break;
    }
    case MQTT_EVENT_PUBLISHED: {
        sp_mqtt_progress_pub_result_t pub;
        progress_lock();
        pub = sp_mqtt_progress_on_published(&s_progress, event->msg_id, now_ms());
        progress_unlock();
        if (pub == SP_MQTT_PROGRESS_PUB_VERIFIED) {
            arm_progress_timer();
            sp_connectivity_note_mqtt_verified();
        }
        /* PARKED_EARLY: deadline refreshes when note_qos1 observes the park. */
        if (s_pub_events != NULL && event->msg_id == s_wait_msg_id && s_wait_msg_id > 0) {
            xEventGroupSetBits(s_pub_events, PUBACK_BIT);
        }
        break;
    }
    default:
        break;
    }
}

esp_err_t sp_mqtt_start(const sp_effective_t *cfg, sp_command_cb_t on_command, void *ctx)
{
    s_cfg = cfg;
    s_cmd = on_command;
    s_ctx = ctx;
    s_connected = false;
    sp_connectivity_note_mqtt_connected(false);
    s_disconnect_handled = false;
    s_wait_msg_id = 0;
    if (s_pub_events == NULL) {
        s_pub_events = xEventGroupCreate();
    }
    if (s_progress_mu == NULL) {
        s_progress_mu = xSemaphoreCreateMutex();
    }
    sp_backoff_config_t bo_cfg;
    sp_backoff_config_mqtt(&bo_cfg);
    sp_backoff_init(&s_backoff, &bo_cfg);
    progress_lock();
    sp_mqtt_progress_init(&s_progress);
    progress_unlock();
    sp_connectivity_set_mqtt_hooks(
        hook_progress_age,
        hook_progress_expired,
        hook_next_attempt_in,
        hook_last_verified
    );

    if (!cfg->network.mqtt_use_tls || cfg->network.mqtt_username[0] == '\0' ||
        cfg->network.mqtt_password[0] == '\0') {
        ESP_LOGE(TAG, "refusing MQTT connection without TLS and per-node credentials");
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg->identity.node_id[0] == '\0') {
        ESP_LOGE(TAG, "refusing MQTT connection without a node id for the client id");
        return ESP_ERR_INVALID_STATE;
    }

    /* .local is stored as an address only until the next client recreate.
     * Public names stay as DNS names so TLS still matches the certificate. */
    strncpy(
        s_configured_host,
        cfg->network.mqtt_host[0] ? cfg->network.mqtt_host : "hub.local",
        sizeof(s_configured_host) - 1
    );
    s_configured_host[sizeof(s_configured_host) - 1] = '\0';
    s_mqtt_port = cfg->network.mqtt_port;
    strncpy(s_client_id, cfg->identity.node_id, sizeof(s_client_id) - 1);
    s_client_id[sizeof(s_client_id) - 1] = '\0';
    if (rebuild_broker_uri() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT URI is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    sp_net_dispatch_start();
    sp_net_dispatch_bind(SP_NET_OP_MQTT_RECONNECT, mqtt_worker_reconnect);
    sp_net_dispatch_bind(SP_NET_OP_MQTT_WATCHDOG, mqtt_worker_watchdog);
    sp_net_dispatch_bind(SP_NET_OP_MQTT_PROGRESS, mqtt_worker_progress);
    sp_connectivity_start();

    esp_err_t open_err = mqtt_open_client();
    if (open_err != ESP_OK) {
        return open_err;
    }
    ESP_LOGI(TAG, "starting client_id=%s uri=%s (exponential reconnect backoff)", s_client_id, s_uri);
    sp_backoff_set_in_flight(&s_backoff, true);
    sp_diag_note_mqtt_connect_attempt();
    arm_connect_watchdog();
    return esp_mqtt_client_start(s_client);
}

esp_mqtt_client_handle_t sp_mqtt_handle(void)
{
    return s_client;
}

bool sp_mqtt_is_connected(void)
{
    return s_connected && s_client != NULL;
}

void sp_mqtt_request_reconnect(void)
{
    if (s_client == NULL || s_connected || sp_lifecycle_ota_reboot_pending()) {
        return;
    }
    if (sp_backoff_is_in_flight(&s_backoff)) {
        if (s_connect_watchdog == NULL) {
            arm_connect_watchdog();
        }
        return;
    }
    if (s_reconnect_timer == NULL) {
        schedule_reconnect();
    }
}

esp_err_t sp_mqtt_publish_json(const char *topic, const char *json, int qos)
{
    if (!s_client) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, json, 0, qos, 0);
    if (msg_id > 0 && qos > 0) {
        sp_mqtt_progress_note_result_t note;
        progress_lock();
        note = sp_mqtt_progress_note_qos1(&s_progress, msg_id, now_ms());
        progress_unlock();
        if (note == SP_MQTT_PROGRESS_NOTE_VERIFIED_EARLY) {
            arm_progress_timer();
            sp_connectivity_note_mqtt_verified();
        }
    } else if (qos <= 0) {
        progress_lock();
        sp_mqtt_progress_note_qos0(&s_progress);
        progress_unlock();
    }
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t sp_mqtt_publish_json_wait(
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
    if (msg_id > 0 && qos > 0) {
        sp_mqtt_progress_note_result_t note;
        progress_lock();
        note = sp_mqtt_progress_note_qos1(&s_progress, msg_id, now_ms());
        progress_unlock();
        if (note == SP_MQTT_PROGRESS_NOTE_VERIFIED_EARLY) {
            arm_progress_timer();
            sp_connectivity_note_mqtt_verified();
        }
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

void sp_mqtt_prepare_ota_reboot(void)
{
    cancel_timer(&s_reconnect_timer);
    cancel_timer(&s_stable_timer);
    cancel_timer(&s_connect_watchdog);
    cancel_timer(&s_progress_timer);
    sp_backoff_set_in_flight(&s_backoff, false);
    if (s_client == NULL || !s_connected) {
        return;
    }
    /* Best-effort quick disconnect; DISCONNECTED is treated as expected teardown. */
    esp_err_t err = esp_mqtt_client_disconnect(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA reboot disconnect request failed: %s", esp_err_to_name(err));
    }
}
