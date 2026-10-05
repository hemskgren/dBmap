#include "dbmap_nvs.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

static esp_err_t get_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t sz = len;
    esp_err_t err = nvs_get_str(h, key, out, &sz);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return err;
}

esp_err_t dbmap_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t dbmap_nvs_load(dbmap_effective_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open("identity", NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "node_id", cfg->identity.node_id, sizeof(cfg->identity.node_id));
        get_str(h, "hw_rev", cfg->identity.hardware_revision, sizeof(cfg->identity.hardware_revision));
        get_str(h, "prov_state", cfg->identity.provisioning_state, sizeof(cfg->identity.provisioning_state));
        nvs_close(h);
    }
    if (nvs_open("network", NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "ssid", cfg->network.ssid, sizeof(cfg->network.ssid));
        get_str(h, "wifi_pw", cfg->network.password, sizeof(cfg->network.password));
        get_str(h, "hub_http", cfg->network.hub_http_url, sizeof(cfg->network.hub_http_url));
        get_str(h, "mqtt_host", cfg->network.mqtt_host, sizeof(cfg->network.mqtt_host));
        get_str(h, "mqtt_user", cfg->network.mqtt_username, sizeof(cfg->network.mqtt_username));
        get_str(h, "mqtt_pw", cfg->network.mqtt_password, sizeof(cfg->network.mqtt_password));
        uint16_t port = 0;
        uint8_t tls = 0;
        if (nvs_get_u16(h, "mqtt_port", &port) == ESP_OK) {
            cfg->network.mqtt_port = port;
        }
        if (nvs_get_u8(h, "mqtt_tls", &tls) == ESP_OK) {
            cfg->network.mqtt_use_tls = tls != 0;
        }
        nvs_close(h);
    }
    if (nvs_open("config", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "cfg_ver", &cfg->config.config_version);
        nvs_get_u32(h, "cal_ver", &cfg->config.calibration_version);
        get_str(h, "clf_ver", cfg->config.classifier_version, sizeof(cfg->config.classifier_version));
        nvs_close(h);
    }
    if (nvs_open("identity", NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "boot_tok", cfg->bootstrap_token, sizeof(cfg->bootstrap_token));
        nvs_close(h);
    }
    cfg->provisioned = cfg->identity.node_id[0] != '\0' &&
                       strcmp(cfg->identity.provisioning_state, "pending") != 0;
    dbmap_rebuild_topics(cfg);
    return ESP_OK;
}

esp_err_t dbmap_nvs_save_identity_network(const dbmap_effective_t *cfg)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("identity", NVS_READWRITE, &h));
    nvs_set_str(h, "node_id", cfg->identity.node_id);
    nvs_set_str(h, "hw_rev", cfg->identity.hardware_revision);
    nvs_set_str(h, "prov_state", cfg->identity.provisioning_state);
    nvs_commit(h);
    nvs_close(h);

    ESP_ERROR_CHECK(nvs_open("network", NVS_READWRITE, &h));
    nvs_set_str(h, "ssid", cfg->network.ssid);
    nvs_set_str(h, "wifi_pw", cfg->network.password);
    nvs_set_str(h, "hub_http", cfg->network.hub_http_url);
    nvs_set_str(h, "mqtt_host", cfg->network.mqtt_host);
    nvs_set_u16(h, "mqtt_port", cfg->network.mqtt_port);
    nvs_set_u8(h, "mqtt_tls", cfg->network.mqtt_use_tls ? 1 : 0);
    nvs_set_str(h, "mqtt_user", cfg->network.mqtt_username);
    nvs_set_str(h, "mqtt_pw", cfg->network.mqtt_password);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t dbmap_nvs_save_bootstrap_token(const char *token)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("identity", NVS_READWRITE, &h));
    nvs_set_str(h, "boot_tok", token);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}
