#include "dbmap_mqtt.h"

#include <stdio.h>
#include <string.h>

#include "dbmap_resolve.h"
#include "esp_log.h"

extern const uint8_t root_ca_pem_start[] asm("_binary_root_ca_pem_start");

static const char *TAG = "dbmap_mqtt";
static esp_mqtt_client_handle_t s_client;
static dbmap_command_cb_t s_cmd;
static void *s_ctx;
static const dbmap_effective_t *s_cfg;
static bool s_connected;
static char s_uri[192];
static char s_client_id[DBMAP_NODE_ID_MAX];

static void mqtt_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected");
        if (s_cfg) {
            esp_mqtt_client_subscribe(s_client, s_cfg->topics.command, 1);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
        break;
    case MQTT_EVENT_ERROR:
        s_connected = false;
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
        .network.reconnect_timeout_ms = 5000,
        .buffer.size = 2048,
    };
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    ESP_LOGI(TAG, "starting client_id=%s uri=%s", s_client_id, s_uri);
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
