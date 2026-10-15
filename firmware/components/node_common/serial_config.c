#include "dbmap_serial_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_timer.h"
#include "dbmap_nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SERIAL_FRAME_MAX 1024
#define SERIAL_RX_BUFFER_SIZE 2048

static const char *TAG = "dbmap_serial";

static bool copy_json_string(const cJSON *object, const char *key, char *out, size_t out_size, bool allow_empty)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value) || value->valuestring == NULL) return false;
    size_t len = strlen(value->valuestring);
    if ((!allow_empty && len == 0) || len >= out_size) return false;
    memcpy(out, value->valuestring, len + 1);
    return true;
}

static void result(const char *request_id, const char *status, const char *error)
{
    char safe_id[65] = "";
    if (request_id != NULL) {
        size_t len = strlen(request_id);
        if (len <= 64) {
            size_t i = 0;
            for (; i < len; ++i) {
                char ch = request_id[i];
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '-')) break;
                safe_id[i] = ch;
            }
            if (i != len) safe_id[0] = '\0';
        }
    }
    printf("DBMAP_PROVISION_RESULT:{\"request_id\":\"%s\",\"status\":\"%s\",\"error\":\"%s\"}\n",
           safe_id, status, error != NULL ? error : "");
    fflush(stdout);
}

static bool valid_request_id(const char *request_id)
{
    size_t len = strlen(request_id);
    if (len == 0 || len > 64) return false;
    for (size_t i = 0; i < len; ++i) {
        char ch = request_id[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-')) return false;
    }
    return true;
}

esp_err_t dbmap_serial_config_wait(dbmap_effective_t *cfg, int timeout_seconds)
{
    esp_err_t uart_err = uart_driver_install(UART_NUM_0, SERIAL_RX_BUFFER_SIZE, 0, 0, NULL, 0);
    if (uart_err != ESP_OK && uart_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "could not enable UART console input: %s", esp_err_to_name(uart_err));
        return uart_err;
    }
    uart_vfs_dev_use_driver(UART_NUM_0);
    ESP_LOGI(TAG, "waiting for local USB provisioning configuration");
    char frame[SERIAL_FRAME_MAX];
    size_t frame_len = 0;
    bool frame_too_large = false;
    int64_t started_us = esp_timer_get_time();
    while (true) {
        uint8_t byte;
        int received = uart_read_bytes(UART_NUM_0, &byte, 1, pdMS_TO_TICKS(1000));
        if (received < 0) return ESP_FAIL;
        if (received == 0) {
            puts("DBMAP_PROVISION_READY");
            fflush(stdout);
            if (timeout_seconds > 0 &&
                esp_timer_get_time() - started_us >= (int64_t)timeout_seconds * 1000000) {
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (byte != '\n') {
            if (byte == '\r' || frame_too_large) continue;
            if (frame_len + 1 >= sizeof(frame)) {
                frame_too_large = true;
                continue;
            }
            frame[frame_len++] = (char)byte;
            continue;
        }
        if (frame_too_large) {
            result("", "error", "frame_too_large");
            frame_len = 0;
            frame_too_large = false;
            continue;
        }
        size_t received_len = frame_len;
        frame[frame_len] = '\0';
        frame_len = 0;
        cJSON *request = cJSON_Parse(frame);
        if (!cJSON_IsObject(request)) {
            const char *parse_error = cJSON_GetErrorPtr();
            int error_offset = parse_error != NULL ? (int)(parse_error - frame) : -1;
            ESP_LOGW(TAG,
                     "invalid USB config JSON (length=%u first=0x%02x last=0x%02x error_offset=%d)",
                     (unsigned)received_len,
                     received_len > 0 ? (unsigned char)frame[0] : 0,
                     received_len > 0 ? (unsigned char)frame[received_len - 1] : 0,
                     error_offset);
            cJSON_Delete(request);
            result("", "error", "invalid_json");
            continue;
        }

        const cJSON *request_id_item = cJSON_GetObjectItemCaseSensitive(request, "request_id");
        char request_id[65] = "";
        if (cJSON_IsString(request_id_item) && request_id_item->valuestring != NULL) {
            size_t id_len = strlen(request_id_item->valuestring);
            if (id_len <= 64) memcpy(request_id, request_id_item->valuestring, id_len + 1);
        }
        const cJSON *protocol = cJSON_GetObjectItemCaseSensitive(request, "protocol");
        const cJSON *operation = cJSON_GetObjectItemCaseSensitive(request, "operation");
        const cJSON *configuration = cJSON_GetObjectItemCaseSensitive(request, "configuration");
        if (!cJSON_IsNumber(protocol) || protocol->valueint != 1 || !cJSON_IsString(operation) ||
            strcmp(operation->valuestring, "configure") != 0 || !cJSON_IsObject(configuration) ||
            !valid_request_id(request_id)) {
            result(request_id, "error", "unsupported_request");
            cJSON_Delete(request);
            continue;
        }

        dbmap_effective_t candidate = *cfg;
        bool valid = copy_json_string(configuration, "ssid", candidate.network.ssid,
                                      sizeof(candidate.network.ssid), false) &&
                     copy_json_string(configuration, "wifi_password", candidate.network.password,
                                      sizeof(candidate.network.password), true) &&
                     copy_json_string(configuration, "hub_url", candidate.network.hub_http_url,
                                      sizeof(candidate.network.hub_http_url), false) &&
                     copy_json_string(configuration, "bootstrap_token", candidate.bootstrap_token,
                                      sizeof(candidate.bootstrap_token), false) &&
                     strncmp(candidate.network.hub_http_url, "https://", 8) == 0 &&
                     candidate.network.hub_http_url[8] != '\0';
        cJSON_Delete(request);
        if (!valid) {
            result(request_id, "error", "invalid_configuration");
            continue;
        }

        esp_err_t err = dbmap_nvs_save_identity_network(&candidate);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not persist USB provisioning configuration: %s", esp_err_to_name(err));
            result(request_id, "error", "nvs_write_failed");
            continue;
        }
        *cfg = candidate;
        result(request_id, "accepted", "");
        return ESP_OK;
    }
    return ESP_FAIL;
}
