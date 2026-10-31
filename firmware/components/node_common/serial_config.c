#include "dbmap_serial_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_timer.h"
#include "dbmap_nvs.h"
#include "dbmap_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SERIAL_FRAME_MAX 1024
#define SERIAL_RX_BUFFER_SIZE 2048
/** Rate-limit repeated UART / recovery error lines (microseconds). */
#define SERIAL_ERR_LOG_INTERVAL_US (30000000LL)

static const char *TAG = "dbmap_serial";

static bool s_uart_ready;
static bool s_uart_init_failed;
static int64_t s_uart_err_log_us;
static uint32_t s_uart_err_suppressed;
static int64_t s_recovery_err_log_us;
static uint32_t s_recovery_err_suppressed;

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

static bool valid_hub_id(const char *hub_id)
{
    size_t len = strlen(hub_id);
    if (len == 0 || len >= DBMAP_HUB_ID_MAX) return false;
    char first = hub_id[0];
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
          (first >= '0' && first <= '9'))) {
        return false;
    }
    for (size_t i = 1; i < len; ++i) {
        char ch = hub_id[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

static void log_uart_error_rate_limited(const char *detail, esp_err_t err)
{
    const int64_t now = esp_timer_get_time();
    if (s_uart_err_log_us == 0 || (now - s_uart_err_log_us) >= SERIAL_ERR_LOG_INTERVAL_US) {
        if (s_uart_err_suppressed > 0) {
            ESP_LOGW(
                TAG,
                "UART console init still failing (%s); suppressed %lu duplicate log(s)",
                esp_err_to_name(err),
                (unsigned long)s_uart_err_suppressed
            );
            s_uart_err_suppressed = 0;
        } else {
            ESP_LOGE(TAG, "%s: %s", detail, esp_err_to_name(err));
        }
        s_uart_err_log_us = now;
    } else {
        s_uart_err_suppressed++;
    }
}

/**
 * Idempotent UART0 driver + VFS console input for USB provisioning.
 * ESP-IDF returns ESP_FAIL (and logs "UART driver already installed") when the
 * driver is present — not ESP_ERR_INVALID_STATE — so callers must check
 * uart_is_driver_installed() before uart_driver_install().
 */
static esp_err_t ensure_uart_console(void)
{
    if (s_uart_ready) {
        return ESP_OK;
    }
    if (s_uart_init_failed) {
        log_uart_error_rate_limited("could not enable UART console input", ESP_FAIL);
        return ESP_FAIL;
    }

    if (!uart_is_driver_installed(UART_NUM_0)) {
        esp_err_t uart_err = uart_driver_install(UART_NUM_0, SERIAL_RX_BUFFER_SIZE, 0, 0, NULL, 0);
        if (uart_err != ESP_OK) {
            /* Race: another path installed between the check and install. */
            if (!uart_is_driver_installed(UART_NUM_0)) {
                s_uart_init_failed = true;
                log_uart_error_rate_limited("could not enable UART console input", uart_err);
                return uart_err;
            }
        }
    }

    uart_vfs_dev_use_driver(UART_NUM_0);
    s_uart_ready = true;
    s_uart_init_failed = false;
    s_uart_err_suppressed = 0;
    return ESP_OK;
}

esp_err_t dbmap_serial_config_wait(dbmap_effective_t *cfg, int timeout_seconds)
{
    esp_err_t uart_err = ensure_uart_console();
    if (uart_err != ESP_OK) {
        return uart_err;
    }
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
                     copy_json_string(configuration, "mqtt_profile", candidate.network.mqtt_profile,
                                      sizeof(candidate.network.mqtt_profile), false) &&
                     strncmp(candidate.network.hub_http_url, "https://", 8) == 0 &&
                     candidate.network.hub_http_url[8] != '\0';
        valid = valid && (strcmp(candidate.network.mqtt_profile, "local") == 0 ||
                          strcmp(candidate.network.mqtt_profile, "external") == 0);

        /* Hub ID is optional for older browsers. When present, persist the Local Hub that
         * issued this USB configuration so inspect/logs can show association before HTTPS
         * provision completes. When omitted, keep any existing NVS value. */
        const cJSON *hub_id_item = cJSON_GetObjectItemCaseSensitive(configuration, "hub_id");
        if (cJSON_IsString(hub_id_item) && hub_id_item->valuestring != NULL &&
            hub_id_item->valuestring[0] != '\0') {
            if (!valid_hub_id(hub_id_item->valuestring) ||
                !copy_json_string(configuration, "hub_id", candidate.identity.hub_id,
                                  sizeof(candidate.identity.hub_id), false)) {
                valid = false;
            }
        }

        /* Bootstrap token is optional. When omitted or empty, keep the existing NVS value. */
        const cJSON *token_item = cJSON_GetObjectItemCaseSensitive(configuration, "bootstrap_token");
        if (cJSON_IsString(token_item) && token_item->valuestring != NULL && token_item->valuestring[0] != '\0') {
            valid = valid && copy_json_string(configuration, "bootstrap_token", candidate.bootstrap_token,
                                              sizeof(candidate.bootstrap_token), false);
        }

        /* Optional MQTT endpoint override so a provisioned device can switch local/external
         * route without redeeming a new bootstrap token. Existing credentials are kept. */
        const cJSON *mqtt_host_item = cJSON_GetObjectItemCaseSensitive(configuration, "mqtt_host");
        if (cJSON_IsString(mqtt_host_item) && mqtt_host_item->valuestring != NULL) {
            valid = valid && copy_json_string(configuration, "mqtt_host", candidate.network.mqtt_host,
                                              sizeof(candidate.network.mqtt_host), false);
        }
        const cJSON *mqtt_port_item = cJSON_GetObjectItemCaseSensitive(configuration, "mqtt_port");
        if (cJSON_IsNumber(mqtt_port_item)) {
            int port = mqtt_port_item->valueint;
            if (port < 1 || port > 65535) {
                valid = false;
            } else {
                candidate.network.mqtt_port = (uint16_t)port;
                candidate.network.mqtt_use_tls = true;
            }
        }

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
        /* Let the accepted result flush over USB, then reboot so Wi-Fi / Hub
         * call-home always starts from the new NVS configuration. */
        ESP_LOGI(TAG, "USB configuration accepted; restarting");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return ESP_OK;
    }
    return ESP_FAIL;
}

static void log_recovery_slice_error_rate_limited(esp_err_t cfg_err)
{
    const int64_t now = esp_timer_get_time();
    if (s_recovery_err_log_us == 0 || (now - s_recovery_err_log_us) >= SERIAL_ERR_LOG_INTERVAL_US) {
        if (s_recovery_err_suppressed > 0) {
            ESP_LOGW(
                TAG,
                "USB recovery slice still failing (%s); suppressed %lu duplicate log(s)",
                esp_err_to_name(cfg_err),
                (unsigned long)s_recovery_err_suppressed
            );
            s_recovery_err_suppressed = 0;
        } else {
            ESP_LOGW(TAG, "USB recovery slice ended: %s", esp_err_to_name(cfg_err));
        }
        s_recovery_err_log_us = now;
    } else {
        s_recovery_err_suppressed++;
    }
}

esp_err_t dbmap_boot_wait_wifi_ip_or_usb(dbmap_effective_t *cfg, int timeout_seconds)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool bounded = timeout_seconds > 0;
    if (bounded) {
        ESP_LOGW(
            TAG,
            "Wi-Fi not up yet; waiting up to %d s for IP (USB configure still accepted)",
            timeout_seconds
        );
    } else {
        ESP_LOGW(
            TAG,
            "Wi-Fi not up yet; waiting for IP indefinitely (USB configure still accepted; no reboot for missing AP)"
        );
    }
    const int64_t started_us = esp_timer_get_time();
    const int64_t limit_us = bounded ? ((int64_t)timeout_seconds * 1000000) : 0;
    while (!dbmap_wifi_has_ip()) {
        int64_t elapsed_us = esp_timer_get_time() - started_us;
        if (bounded && elapsed_us >= limit_us) {
            return ESP_ERR_TIMEOUT;
        }
        int slice_s = 5;
        if (bounded) {
            int remain_s = (int)((limit_us - elapsed_us + 999999) / 1000000);
            if (remain_s < 1) {
                remain_s = 1;
            }
            slice_s = remain_s > 5 ? 5 : remain_s;
        }
        /* Short slices so a late GOT_IP is noticed without ending the USB window early. */
        esp_err_t cfg_err = dbmap_serial_config_wait(cfg, slice_s);
        if (cfg_err == ESP_OK) {
            /* Accepted configure reboots inside serial_config_wait. */
            return ESP_OK;
        }
        if (cfg_err == ESP_ERR_TIMEOUT) {
            continue;
        }
        /* UART init failure must not spin: yield so the idle task feeds the WDT
         * while background Wi-Fi reconnect timers keep running. */
        log_recovery_slice_error_rate_limited(cfg_err);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Wi-Fi IP available; continuing boot");
    return ESP_OK;
}
