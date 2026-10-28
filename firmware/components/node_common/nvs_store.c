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
        get_str(h, "hub_id", cfg->identity.hub_id, sizeof(cfg->identity.hub_id));
        get_str(h, "hw_rev", cfg->identity.hardware_revision, sizeof(cfg->identity.hardware_revision));
        get_str(h, "prov_state", cfg->identity.provisioning_state, sizeof(cfg->identity.provisioning_state));
        nvs_close(h);
    }
    if (nvs_open("network", NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "ssid", cfg->network.ssid, sizeof(cfg->network.ssid));
        get_str(h, "wifi_pw", cfg->network.password, sizeof(cfg->network.password));
        get_str(h, "hub_http", cfg->network.hub_http_url, sizeof(cfg->network.hub_http_url));
        get_str(h, "mqtt_profile", cfg->network.mqtt_profile, sizeof(cfg->network.mqtt_profile));
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
        get_str(h, "api_tok", cfg->device_api_token, sizeof(cfg->device_api_token));
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
    esp_err_t err = nvs_open("identity", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "node_id", cfg->identity.node_id);
    if (err == ESP_OK) err = nvs_set_str(h, "hub_id", cfg->identity.hub_id);
    if (err == ESP_OK) err = nvs_set_str(h, "hw_rev", cfg->identity.hardware_revision);
    if (err == ESP_OK) err = nvs_set_str(h, "prov_state", cfg->identity.provisioning_state);
    if (err == ESP_OK) err = nvs_set_str(h, "boot_tok", cfg->bootstrap_token);
    /* USB setup runs before the Hub issues a token. Do not plant or wipe api_tok with an empty value. */
    if (err == ESP_OK && cfg->device_api_token[0] != '\0') {
        err = nvs_set_str(h, "api_tok", cfg->device_api_token);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    err = nvs_open("network", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "ssid", cfg->network.ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "wifi_pw", cfg->network.password);
    if (err == ESP_OK) err = nvs_set_str(h, "hub_http", cfg->network.hub_http_url);

    if (err == ESP_OK) err = nvs_set_str(h, "mqtt_profile", cfg->network.mqtt_profile);

    if (err == ESP_OK) err = nvs_set_str(h, "mqtt_host", cfg->network.mqtt_host);
    if (err == ESP_OK) err = nvs_set_u16(h, "mqtt_port", cfg->network.mqtt_port);
    if (err == ESP_OK) err = nvs_set_u8(h, "mqtt_tls", cfg->network.mqtt_use_tls ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(h, "mqtt_user", cfg->network.mqtt_username);
    if (err == ESP_OK) err = nvs_set_str(h, "mqtt_pw", cfg->network.mqtt_password);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t dbmap_nvs_save_bootstrap_token(const char *token)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("identity", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "boot_tok", token);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t dbmap_nvs_ota_pending_save(const dbmap_ota_pending_t *pending)
{
    if (pending == NULL || !pending->pending) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open("ota", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "prev", pending->previous_version);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "tgt", pending->target_version);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "fid", pending->firmware_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "pend", 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t dbmap_nvs_ota_pending_load(dbmap_ota_pending_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK;
    }
    uint8_t pend = 0;
    if (nvs_get_u8(h, "pend", &pend) != ESP_OK || pend == 0) {
        nvs_close(h);
        return ESP_OK;
    }
    get_str(h, "prev", out->previous_version, sizeof(out->previous_version));
    get_str(h, "tgt", out->target_version, sizeof(out->target_version));
    get_str(h, "fid", out->firmware_id, sizeof(out->firmware_id));
    nvs_close(h);
    out->pending = out->previous_version[0] != '\0' && out->target_version[0] != '\0';
    return ESP_OK;
}

esp_err_t dbmap_nvs_ota_pending_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("ota", NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
