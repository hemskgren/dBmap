#include "dbmap_provision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "dbmap_nvs.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "dbmap_prov";
extern const uint8_t root_ca_pem_start[] asm("_binary_root_ca_pem_start");

static esp_err_t collect_body(esp_http_client_handle_t client, char *buf, int buf_len)
{
    int total = 0;
    while (total < buf_len - 1) {
        int n = esp_http_client_read(client, buf + total, buf_len - 1 - total);
        if (n < 0) {
            return ESP_FAIL;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    buf[total] = 0;
    return ESP_OK;
}

esp_err_t dbmap_hub_provision(dbmap_effective_t *cfg)
{
    if (cfg->bootstrap_token[0] == '\0') {
        ESP_LOGE(TAG, "no bootstrap token; refusing to connect without provisioning");
        return ESP_ERR_NOT_FOUND;
    }
    if (cfg->provisioned) {
        return ESP_OK;
    }

    if (strncmp(cfg->network.hub_http_url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "bootstrap URL must use HTTPS");
        return ESP_ERR_INVALID_ARG;
    }

    char url[192];
    snprintf(url, sizeof(url), "%s/api/v1/provision", cfg->network.hub_http_url);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "bootstrap_token", cfg->bootstrap_token);
    cJSON_AddStringToObject(req, "hardware_revision", cfg->identity.hardware_revision);
    cJSON_AddStringToObject(req, "firmware_version", CONFIG_DBMAP_FIRMWARE_VERSION);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        .cert_pem = (const char *)root_ca_pem_start,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));
    esp_err_t err = esp_http_client_open(client, (int)strlen(body));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "provision connect failed: %s", esp_err_to_name(err));
        free(body);
        esp_http_client_cleanup(client);
        return err;
    }
    int written = esp_http_client_write(client, body, (int)strlen(body));
    free(body);
    if (written < 0) {
        ESP_LOGE(TAG, "provision request write failed");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    (void)esp_http_client_fetch_headers(client);

    char resp[1024];
    if (collect_body(client, resp, sizeof(resp)) != ESP_OK) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (status != 200) {
        ESP_LOGE(TAG, "provision HTTP %d: %s", status, resp);
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(resp);
    if (!json) {
        return ESP_FAIL;
    }
    const cJSON *node_id = cJSON_GetObjectItem(json, "node_id");
    const cJSON *mqtt = cJSON_GetObjectItem(json, "mqtt");
    const cJSON *topics = cJSON_GetObjectItem(json, "topics");
    if (!cJSON_IsString(node_id) || !cJSON_IsObject(mqtt)) {
        cJSON_Delete(json);
        return ESP_FAIL;
    }
    const cJSON *tls = cJSON_GetObjectItem(mqtt, "use_tls");
    if (!cJSON_IsTrue(tls)) {
        ESP_LOGE(TAG, "hub returned a non-TLS MQTT endpoint; refusing insecure configuration");
        cJSON_Delete(json);
        return ESP_ERR_INVALID_STATE;
    }
    strncpy(cfg->identity.node_id, node_id->valuestring, sizeof(cfg->identity.node_id) - 1);
    strncpy(cfg->identity.provisioning_state, "provisioned", sizeof(cfg->identity.provisioning_state) - 1);

    const cJSON *host = cJSON_GetObjectItem(mqtt, "host");
    const cJSON *port = cJSON_GetObjectItem(mqtt, "port");
    const cJSON *user = cJSON_GetObjectItem(mqtt, "username");
    const cJSON *pass = cJSON_GetObjectItem(mqtt, "password");
    if (cJSON_IsString(host)) {
        strncpy(cfg->network.mqtt_host, host->valuestring, sizeof(cfg->network.mqtt_host) - 1);
    }
    if (cJSON_IsNumber(port)) {
        cfg->network.mqtt_port = (uint16_t)port->valuedouble;
    }
    cfg->network.mqtt_use_tls = true;
    if (cJSON_IsString(user)) {
        strncpy(cfg->network.mqtt_username, user->valuestring, sizeof(cfg->network.mqtt_username) - 1);
    }
    if (cJSON_IsString(pass)) {
        strncpy(cfg->network.mqtt_password, pass->valuestring, sizeof(cfg->network.mqtt_password) - 1);
    }
    if (cJSON_IsObject(topics)) {
        const char *names[] = {"hello", "keepalive", "health", "status", "command", "ack", "config"};
        char *slots[] = {
            cfg->topics.hello, cfg->topics.keepalive, cfg->topics.health, cfg->topics.status,
            cfg->topics.command, cfg->topics.ack, cfg->topics.config,
        };
        for (int i = 0; i < 7; ++i) {
            const cJSON *t = cJSON_GetObjectItem(topics, names[i]);
            if (cJSON_IsString(t)) {
                strncpy(slots[i], t->valuestring, DBMAP_TOPIC_MAX - 1);
            }
        }
    } else {
        dbmap_rebuild_topics(cfg);
    }
    cJSON_Delete(json);
    cfg->provisioned = true;
    dbmap_nvs_save_identity_network(cfg);
    ESP_LOGI(TAG, "provisioned as %s", cfg->identity.node_id);
    return ESP_OK;
}
