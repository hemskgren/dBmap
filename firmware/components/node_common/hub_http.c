#include "dbmap_hub_http.h"

#include <stdio.h>
#include <string.h>

#include "dbmap_resolve.h"
#include "esp_log.h"

static const char *TAG = "dbmap_http";
extern const uint8_t root_ca_pem_start[] asm("_binary_root_ca_pem_start");

const char *dbmap_hub_root_ca(void)
{
    return (const char *)root_ca_pem_start;
}

esp_err_t dbmap_hub_format_https_url(const char *hub_http_url, const char *api_path, char *url, size_t url_len)
{
    if (hub_http_url == NULL || api_path == NULL || url == NULL || url_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(hub_http_url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "Hub URL must use HTTPS");
        return ESP_ERR_INVALID_ARG;
    }

    const char *hub_base = hub_http_url;
    char resolved_base[160];
    const char *host_start = hub_base + 8;
    const char *host_end = strpbrk(host_start, ":/");
    size_t host_len = host_end ? (size_t)(host_end - host_start) : strlen(host_start);
    if (host_len > 0 && host_len < 128) {
        char hostname[128];
        memcpy(hostname, host_start, host_len);
        hostname[host_len] = '\0';
        char ip[16];
        if (dbmap_resolve_ipv4(hostname, ip, sizeof(ip)) == ESP_OK) {
            int n = snprintf(resolved_base, sizeof(resolved_base), "https://%s%s", ip, host_end ? host_end : "");
            if (n > 0 && (size_t)n < sizeof(resolved_base)) {
                hub_base = resolved_base;
                ESP_LOGI(TAG, "call-home HTTPS via %s (%s)", ip, hostname);
            }
        } else {
            ESP_LOGW(TAG, "could not resolve %s; attempting original Hub URL", hostname);
        }
    }

    int url_written = snprintf(url, url_len, "%s%s", hub_base, api_path);
    if (url_written <= 0 || (size_t)url_written >= url_len) {
        ESP_LOGE(TAG, "Hub URL is too long");
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
