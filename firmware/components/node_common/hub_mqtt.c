#include "dbmap_mqtt.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

extern const uint8_t root_ca_pem_start[] asm("_binary_root_ca_pem_start");

static const char *TAG = "dbmap_mqtt";
static esp_mqtt_client_handle_t s_client;
static dbmap_command_cb_t s_cmd;
static void *s_ctx;
static const dbmap_effective_t *s_cfg;

static void mqtt_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        if (s_cfg) {
            esp_mqtt_client_subscribe(s_client, s_cfg->topics.command, 1);
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
    const char *host = cfg->network.mqtt_host[0] ? cfg->network.mqtt_host : "hub.local";
    if (!cfg->network.mqtt_use_tls || cfg->network.mqtt_username[0] == '\0' ||
        cfg->network.mqtt_password[0] == '\0') {
        ESP_LOGE(TAG, "refusing MQTT connection without TLS and per-node credentials");
        return ESP_ERR_INVALID_STATE;
    }
    char uri[192];
    snprintf(uri, sizeof(uri), "mqtts://%s:%u", host, cfg->network.mqtt_port);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .broker.verification.certificate = (const char *)root_ca_pem_start,
        .credentials.username = cfg->network.mqtt_username,
        .credentials.authentication.password = cfg->network.mqtt_password,
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 2000,
    };
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    return esp_mqtt_client_start(s_client);
}

esp_mqtt_client_handle_t dbmap_mqtt_handle(void)
{
    return s_client;
}

esp_err_t dbmap_mqtt_publish_json(const char *topic, const char *json, int qos)
{
    if (!s_client) {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, json, 0, qos, 0);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}
