#include "dbmap_ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "dbmap_hub_http.h"
#include "dbmap_lifecycle.h"
#include "dbmap_mqtt.h"
#include "dbmap_nvs.h"
#include "dbmap_wifi.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"

static const char *TAG = "dbmap_ota";
static const uint32_t kOtaStatusAckTimeoutMs = 5000;
static const uint32_t kOtaRebootCleanupMs = 500;
static volatile bool s_in_flight;

static esp_err_t publish_ota_status(
    const dbmap_effective_t *cfg,
    const char *event,
    const char *ota_phase,
    const char *previous_version,
    const char *target_version,
    const char *firmware_id,
    const char *running_version,
    bool wait_ack
)
{
    if (cfg == NULL || cfg->topics.status[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(o, "protocol_version", 1);
    cJSON_AddStringToObject(o, "message_type", "ota_update");
    cJSON_AddStringToObject(o, "event", event);
    cJSON_AddStringToObject(o, "ota_phase", ota_phase);
    cJSON_AddStringToObject(o, "reboot_reason", "ota");
    cJSON_AddStringToObject(o, "node_id", cfg->identity.node_id);
    cJSON_AddStringToObject(o, "firmware_version", running_version);
    cJSON_AddStringToObject(o, "previous_firmware_version", previous_version);
    cJSON_AddStringToObject(o, "target_firmware_version", target_version);
    if (firmware_id != NULL && firmware_id[0] != '\0') {
        cJSON_AddStringToObject(o, "firmware_id", firmware_id);
    }
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err;
    if (wait_ack) {
        err = dbmap_mqtt_publish_json_wait(cfg->topics.status, json, 1, kOtaStatusAckTimeoutMs);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA status published with MQTT PUBACK (%s)", event);
        } else if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "OTA status PUBACK timeout; continuing (%s)", event);
        } else {
            ESP_LOGW(TAG, "OTA status publish failed (%s); continuing (%s)", esp_err_to_name(err), event);
        }
    } else {
        err = dbmap_mqtt_publish_json(cfg->topics.status, json, 1);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OTA status publish failed (%s); continuing (%s)", esp_err_to_name(err), event);
        }
    }
    free(json);
    return err;
}

static void persist_pending_and_reboot(
    const dbmap_effective_t *cfg,
    const char *firmware_id,
    const char *previous_version,
    const char *target_version
)
{
    dbmap_ota_pending_t pending = {0};
    pending.pending = true;
    strncpy(pending.previous_version, previous_version, sizeof(pending.previous_version) - 1);
    strncpy(pending.target_version, target_version, sizeof(pending.target_version) - 1);
    if (firmware_id != NULL) {
        strncpy(pending.firmware_id, firmware_id, sizeof(pending.firmware_id) - 1);
    }

    /* Persist first so a missed pre-reboot publish can still be reported after boot. */
    esp_err_t nvs_err = dbmap_nvs_ota_pending_save(&pending);
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "could not persist pending OTA record (%s); rebooting anyway", esp_err_to_name(nvs_err));
    }

    if (dbmap_mqtt_is_connected()) {
        (void)publish_ota_status(
            cfg,
            "install_succeeded",
            "installed_pending_reboot",
            previous_version,
            target_version,
            firmware_id,
            previous_version,
            true
        );
    } else {
        ESP_LOGW(TAG, "MQTT unavailable before OTA reboot; pending record retained for post-boot report");
    }

    /* After the install report (and bounded PUBACK), freeze reconnects and
     * treat teardown as expected. Cleanup is strictly time-bounded. */
    dbmap_lifecycle_enter_ota_reboot_pending();
    dbmap_mqtt_prepare_ota_reboot();
    dbmap_wifi_prepare_ota_reboot();
    vTaskDelay(pdMS_TO_TICKS(kOtaRebootCleanupMs));

    ESP_LOGI(TAG, "OTA success; restarting into %s (was %s)", target_version, previous_version);
    s_in_flight = false;
    esp_restart();
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool parse_sha256_hex(const char *hex, uint8_t out[32])
{
    if (hex == NULL || strlen(hex) != 64) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void digest_to_hex(const uint8_t digest[32], char out[65])
{
    static const char *kHex = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = kHex[(digest[i] >> 4) & 0xf];
        out[i * 2 + 1] = kHex[digest[i] & 0xf];
    }
    out[64] = '\0';
}

esp_err_t dbmap_ota_apply(
    const dbmap_effective_t *cfg,
    const char *firmware_id,
    const char *firmware_version,
    const char *app_sha256_hex,
    uint32_t app_size_bytes
)
{
    if (cfg == NULL || firmware_id == NULL || firmware_version == NULL || app_sha256_hex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (firmware_id[0] == '\0' || firmware_version[0] == '\0' || app_sha256_hex[0] == '\0' || app_size_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(firmware_version, CONFIG_DBMAP_FIRMWARE_VERSION) == 0) {
        ESP_LOGI(TAG, "OTA refused; already running desired version %s", firmware_version);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t expected_sha[32];
    if (!parse_sha256_hex(app_sha256_hex, expected_sha)) {
        ESP_LOGW(TAG, "OTA refused; invalid app_sha256");
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update == NULL) {
        ESP_LOGE(TAG, "OTA refused; no inactive update partition");
        return ESP_ERR_NOT_FOUND;
    }
    if (app_size_bytes > update->size) {
        ESP_LOGW(
            TAG,
            "OTA refused; image %lu bytes exceeds slot %lu",
            (unsigned long)app_size_bytes,
            (unsigned long)update->size
        );
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_in_flight) {
        ESP_LOGW(TAG, "OTA skipped; update already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    s_in_flight = true;

    ESP_LOGI(
        TAG,
        "OTA start: desired %s id=%s size=%lu sha=%s slot=%s",
        firmware_version,
        firmware_id,
        (unsigned long)app_size_bytes,
        app_sha256_hex,
        update->label
    );

    char url[192];
    esp_err_t err = dbmap_hub_format_https_url(cfg->network.hub_http_url, "/api/v1/device/ota-image", url, sizeof(url));
    if (err != ESP_OK) {
        s_in_flight = false;
        ESP_LOGW(TAG, "OTA failed; could not build download URL");
        return err;
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 60000,
        .cert_pem = dbmap_hub_root_ca(),
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        s_in_flight = false;
        ESP_LOGW(TAG, "OTA failed; HTTP client init");
        return ESP_FAIL;
    }

    char authorization[DBMAP_TOKEN_MAX + 8];
    snprintf(authorization, sizeof(authorization), "Bearer %s", cfg->device_api_token);
    esp_http_client_set_header(client, "Authorization", authorization);

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        s_in_flight = false;
        ESP_LOGW(TAG, "OTA failed; download unreachable (%s)", esp_err_to_name(err));
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        esp_http_client_cleanup(client);
        s_in_flight = false;
        ESP_LOGW(TAG, "OTA failed; download HTTP %d", status);
        return ESP_FAIL;
    }
    if (content_length > 0 && (uint32_t)content_length != app_size_bytes) {
        esp_http_client_cleanup(client);
        s_in_flight = false;
        ESP_LOGW(
            TAG,
            "OTA failed; Content-Length %d != expected %lu",
            content_length,
            (unsigned long)app_size_bytes
        );
        return ESP_ERR_INVALID_SIZE;
    }

    esp_ota_handle_t ota = 0;
    err = esp_ota_begin(update, app_size_bytes, &ota);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        s_in_flight = false;
        ESP_LOGW(TAG, "OTA failed; begin (%s)", esp_err_to_name(err));
        return err;
    }

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    uint8_t buf[4096];
    uint32_t written = 0;
    while (written < app_size_bytes) {
        int to_read = (int)sizeof(buf);
        if (written + (uint32_t)to_read > app_size_bytes) {
            to_read = (int)(app_size_bytes - written);
        }
        int n = esp_http_client_read(client, (char *)buf, to_read);
        if (n < 0) {
            err = ESP_FAIL;
            ESP_LOGW(TAG, "OTA failed; read error after %lu bytes", (unsigned long)written);
            break;
        }
        if (n == 0) {
            err = ESP_ERR_INVALID_SIZE;
            ESP_LOGW(TAG, "OTA failed; short download at %lu/%lu", (unsigned long)written, (unsigned long)app_size_bytes);
            break;
        }
        err = esp_ota_write(ota, buf, (size_t)n);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OTA failed; write (%s)", esp_err_to_name(err));
            break;
        }
        mbedtls_sha256_update(&sha_ctx, buf, (size_t)n);
        written += (uint32_t)n;
    }

    esp_http_client_cleanup(client);

    uint8_t actual_sha[32];
    mbedtls_sha256_finish(&sha_ctx, actual_sha);
    mbedtls_sha256_free(&sha_ctx);

    if (err != ESP_OK || written != app_size_bytes) {
        esp_ota_abort(ota);
        s_in_flight = false;
        ESP_LOGW(TAG, "OTA aborted; keeping running firmware %s", CONFIG_DBMAP_FIRMWARE_VERSION);
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }

    if (memcmp(actual_sha, expected_sha, sizeof(expected_sha)) != 0) {
        char actual_hex[65];
        digest_to_hex(actual_sha, actual_hex);
        esp_ota_abort(ota);
        s_in_flight = false;
        ESP_LOGW(TAG, "OTA aborted; SHA-256 mismatch got %s", actual_hex);
        return ESP_ERR_INVALID_CRC;
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        s_in_flight = false;
        ESP_LOGW(TAG, "OTA aborted; end (%s)", esp_err_to_name(err));
        return err;
    }

    /* Integrity is SHA-256 + esp_ota_end. App descriptor PROJECT_VER must match Desired; otherwise
     * a Desired version bump with a stale firmware_id would boot-loop the same image forever. */
    esp_app_desc_t new_app;
    err = esp_ota_get_partition_description(update, &new_app);
    if (err != ESP_OK) {
        s_in_flight = false;
        ESP_LOGW(TAG, "OTA aborted; could not read app descriptor (%s)", esp_err_to_name(err));
        return err;
    }
    if (strcmp(new_app.version, firmware_version) != 0) {
        s_in_flight = false;
        ESP_LOGW(
            TAG,
            "OTA aborted; image version %s != desired %s (stale firmware_id %s?); not switching boot slot",
            new_app.version,
            firmware_version,
            firmware_id
        );
        return ESP_ERR_INVALID_VERSION;
    }
    if (strcmp(new_app.version, CONFIG_DBMAP_FIRMWARE_VERSION) == 0) {
        s_in_flight = false;
        ESP_LOGW(
            TAG,
            "OTA aborted; image is already running version %s; not switching boot slot",
            new_app.version
        );
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        s_in_flight = false;
        ESP_LOGW(TAG, "OTA aborted; set boot partition (%s)", esp_err_to_name(err));
        return err;
    }

    persist_pending_and_reboot(cfg, firmware_id, CONFIG_DBMAP_FIRMWARE_VERSION, new_app.version);
    return ESP_OK;
}

void dbmap_ota_report_pending_boot(const dbmap_effective_t *cfg)
{
    dbmap_ota_pending_t pending = {0};
    if (dbmap_nvs_ota_pending_load(&pending) != ESP_OK || !pending.pending) {
        return;
    }
    if (!dbmap_mqtt_is_connected()) {
        return;
    }

    const char *running = CONFIG_DBMAP_FIRMWARE_VERSION;
    if (strcmp(running, pending.target_version) == 0) {
        ESP_LOGI(
            TAG,
            "OTA boot confirmed: %s -> %s",
            pending.previous_version,
            running
        );
        (void)publish_ota_status(
            cfg,
            "boot_confirmed",
            "boot_confirmed",
            pending.previous_version,
            pending.target_version,
            pending.firmware_id,
            running,
            true
        );
        (void)dbmap_nvs_ota_pending_clear();
        return;
    }

    ESP_LOGW(
        TAG,
        "OTA pending target %s but running %s (previous %s); clearing pending record",
        pending.target_version,
        running,
        pending.previous_version
    );
    (void)publish_ota_status(
        cfg,
        "boot_mismatch",
        "boot_mismatch",
        pending.previous_version,
        pending.target_version,
        pending.firmware_id,
        running,
        false
    );
    (void)dbmap_nvs_ota_pending_clear();
}

void dbmap_ota_mark_valid_after_mqtt(const dbmap_effective_t *cfg)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return;
    }
    esp_ota_img_states_t state;
    bool pending_verify = false;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        pending_verify = state == ESP_OTA_IMG_PENDING_VERIFY;
    }

    if (pending_verify) {
        ESP_LOGI(TAG, "pending OTA verify; waiting for MQTT before marking app valid");
    }

    for (int i = 0; i < 60; i++) {
        if (dbmap_mqtt_is_connected()) {
            if (pending_verify) {
                esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "OTA image marked valid; rollback cancelled");
                } else {
                    ESP_LOGW(
                        TAG,
                        "could not mark OTA image valid (%s); leaving rollback to bootloader",
                        esp_err_to_name(err)
                    );
                }
            }
            /* Installed != booted: confirm running version against NVS pending record. */
            dbmap_ota_report_pending_boot(cfg);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (pending_verify) {
        ESP_LOGW(TAG, "MQTT not up within 60s after OTA boot; leaving rollback to bootloader");
    }
    /* Keep NVS pending so a later successful MQTT session can still report. */
}
