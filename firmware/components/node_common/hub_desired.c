#include "dbmap_desired.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "dbmap_hub_http.h"
#include "dbmap_ota.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "dbmap_desired";
static const int kPollMs = 60 * 60 * 1000;

enum {
    kLogNone = 0,
    kLogAbsent,
    kLogRejected,
    kLogDown,
    kLogEmpty,
    kLogMatch,
    kLogMismatch,
};

static int s_logged = kLogNone;
static char s_logged_id[64];
static char s_logged_version[32];

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

static void log_once(int kind, const char *firmware_id, const char *firmware_version)
{
    if (s_logged == kind &&
        (firmware_id == NULL || strcmp(s_logged_id, firmware_id) == 0) &&
        (firmware_version == NULL || strcmp(s_logged_version, firmware_version) == 0)) {
        return;
    }
    s_logged = kind;
    s_logged_id[0] = '\0';
    s_logged_version[0] = '\0';
    if (firmware_id != NULL) {
        strncpy(s_logged_id, firmware_id, sizeof(s_logged_id) - 1);
    }
    if (firmware_version != NULL) {
        strncpy(s_logged_version, firmware_version, sizeof(s_logged_version) - 1);
    }
}

static void check_desired(dbmap_effective_t *cfg)
{
    if (cfg->device_api_token[0] == '\0') {
        if (s_logged != kLogAbsent) {
            ESP_LOGW(TAG, "device API credential absent; desired check skipped");
            log_once(kLogAbsent, NULL, NULL);
        }
        return;
    }

    char url[192];
    esp_err_t err = dbmap_hub_format_https_url(cfg->network.hub_http_url, "/api/v1/device/desired-firmware", url, sizeof(url));
    if (err != ESP_OK) {
        if (s_logged != kLogDown) {
            ESP_LOGW(TAG, "desired check skipped; keeping running firmware %s", CONFIG_DBMAP_FIRMWARE_VERSION);
            log_once(kLogDown, NULL, NULL);
        }
        return;
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 8000,
        .cert_pem = dbmap_hub_root_ca(),
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        if (s_logged != kLogDown) {
            ESP_LOGW(TAG, "desired check failed; keeping running firmware %s", CONFIG_DBMAP_FIRMWARE_VERSION);
            log_once(kLogDown, NULL, NULL);
        }
        return;
    }

    char authorization[DBMAP_TOKEN_MAX + 8];
    snprintf(authorization, sizeof(authorization), "Bearer %s", cfg->device_api_token);
    esp_http_client_set_header(client, "Authorization", authorization);
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        if (s_logged != kLogDown) {
            ESP_LOGW(TAG, "desired check unreachable (%s); keeping running firmware %s", esp_err_to_name(err), CONFIG_DBMAP_FIRMWARE_VERSION);
            log_once(kLogDown, NULL, NULL);
        }
        return;
    }
    (void)esp_http_client_fetch_headers(client);
    char body[768];
    if (collect_body(client, body, sizeof(body)) != ESP_OK) {
        esp_http_client_cleanup(client);
        if (s_logged != kLogDown) {
            ESP_LOGW(TAG, "desired check response failed; keeping running firmware %s", CONFIG_DBMAP_FIRMWARE_VERSION);
            log_once(kLogDown, NULL, NULL);
        }
        return;
    }
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (status == 401 || status == 403) {
        if (s_logged != kLogRejected) {
            ESP_LOGW(TAG, "desired check rejected (HTTP %d); keeping running firmware %s", status, CONFIG_DBMAP_FIRMWARE_VERSION);
            log_once(kLogRejected, NULL, NULL);
        }
        return;
    }
    if (status != 200) {
        if (s_logged != kLogDown) {
            ESP_LOGW(TAG, "desired check failed (HTTP %d); keeping running firmware %s", status, CONFIG_DBMAP_FIRMWARE_VERSION);
            log_once(kLogDown, NULL, NULL);
        }
        return;
    }

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) {
        if (s_logged != kLogDown) {
            ESP_LOGW(TAG, "desired check returned invalid JSON; keeping running firmware %s", CONFIG_DBMAP_FIRMWARE_VERSION);
            log_once(kLogDown, NULL, NULL);
        }
        return;
    }
    const cJSON *firmware_id = cJSON_GetObjectItem(json, "firmware_id");
    const cJSON *firmware_version = cJSON_GetObjectItem(json, "firmware_version");
    const cJSON *app_sha256 = cJSON_GetObjectItem(json, "app_sha256");
    const cJSON *app_size_bytes = cJSON_GetObjectItem(json, "app_size_bytes");
    const char *desired_id = cJSON_IsString(firmware_id) ? firmware_id->valuestring : "";
    const char *desired_version = cJSON_IsString(firmware_version) ? firmware_version->valuestring : "";
    const char *desired_sha = cJSON_IsString(app_sha256) ? app_sha256->valuestring : "";
    uint32_t desired_size = 0;
    if (cJSON_IsNumber(app_size_bytes) && app_size_bytes->valuedouble > 0) {
        desired_size = (uint32_t)app_size_bytes->valuedouble;
    }
    if (desired_version[0] == '\0') {
        if (s_logged != kLogEmpty) {
            ESP_LOGI(TAG, "desired firmware not set; running %s", CONFIG_DBMAP_FIRMWARE_VERSION);
            log_once(kLogEmpty, NULL, NULL);
        }
        cJSON_Delete(json);
        return;
    }
    if (strcmp(desired_version, CONFIG_DBMAP_FIRMWARE_VERSION) == 0) {
        if (s_logged != kLogMatch || strcmp(s_logged_version, desired_version) != 0) {
            ESP_LOGI(TAG, "desired firmware matches running %s", CONFIG_DBMAP_FIRMWARE_VERSION);
            log_once(kLogMatch, desired_id, desired_version);
        }
        cJSON_Delete(json);
        return;
    }

    if (s_logged != kLogMismatch || strcmp(s_logged_id, desired_id) != 0 || strcmp(s_logged_version, desired_version) != 0) {
        ESP_LOGW(
            TAG,
            "desired firmware differs: running %s, desired %s, firmware_id %s",
            CONFIG_DBMAP_FIRMWARE_VERSION,
            desired_version,
            desired_id[0] != '\0' ? desired_id : "(none)"
        );
        log_once(kLogMismatch, desired_id, desired_version);
    }

    if (desired_id[0] != '\0' && desired_sha[0] != '\0' && desired_size > 0) {
        ESP_LOGI(TAG, "starting OTA for desired %s (%s)", desired_version, desired_id);
        esp_err_t ota_err = dbmap_ota_apply(cfg, desired_id, desired_version, desired_sha, desired_size);
        if (ota_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "OTA failed (%s); keeping running firmware %s",
                esp_err_to_name(ota_err),
                CONFIG_DBMAP_FIRMWARE_VERSION
            );
        }
    } else {
        ESP_LOGW(
            TAG,
            "desired mismatch without usable id/sha/size (id=%s); keeping running firmware %s",
            desired_id[0] != '\0' ? desired_id : "(none)",
            CONFIG_DBMAP_FIRMWARE_VERSION
        );
    }
    cJSON_Delete(json);
}

static void poll_task(void *arg)
{
    dbmap_effective_t *cfg = arg;
    while (1) {
        check_desired(cfg);
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

void dbmap_desired_poll_start(dbmap_effective_t *cfg)
{
    if (cfg == NULL || !cfg->provisioned) {
        return;
    }
    if (xTaskCreate(poll_task, "desired", 12288, cfg, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start desired firmware check");
    }
}
