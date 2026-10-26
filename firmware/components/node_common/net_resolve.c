#include "dbmap_resolve.h"

#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"

static const char *TAG = "dbmap_resolve";

static bool is_local_hostname(const char *hostname, size_t len)
{
    return len > 6 && strcasecmp(hostname + len - 6, ".local") == 0;
}

static esp_err_t format_ipv4(const esp_ip4_addr_t *addr, char *ip_out, size_t ip_out_len)
{
    int n = snprintf(ip_out, ip_out_len, IPSTR, IP2STR(addr));
    if (n <= 0 || (size_t)n >= ip_out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t resolve_mdns_ipv4(const char *hostname, char *ip_out, size_t ip_out_len)
{
    size_t len = strlen(hostname);
    char short_name[64];
    if (len >= sizeof(short_name)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(short_name, hostname, len + 1);
    if (is_local_hostname(short_name, len)) {
        short_name[len - 6] = '\0';
    }
    if (short_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_ip4_addr_t addr = {0};
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        err = mdns_query_a(short_name, 3000, &addr);
        if (err == ESP_OK) {
            err = format_ipv4(&addr, ip_out, ip_out_len);
            if (err != ESP_OK) {
                return err;
            }
            ESP_LOGI(TAG, "mDNS %s -> %s (attempt %d)", hostname, ip_out, attempt);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "mDNS lookup for %s failed on attempt %d: %s", hostname, attempt, esp_err_to_name(err));
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t resolve_dns_ipv4(const char *hostname, char *ip_out, size_t ip_out_len)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(hostname, NULL, &hints, &res);
    if (rc != 0 || res == NULL) {
        ESP_LOGW(TAG, "getaddrinfo(%s) failed: %d", hostname, rc);
        if (res) {
            freeaddrinfo(res);
        }
        return ESP_ERR_NOT_FOUND;
    }
    const struct sockaddr_in *sa = (const struct sockaddr_in *)res->ai_addr;
    esp_ip4_addr_t addr = { .addr = sa->sin_addr.s_addr };
    esp_err_t err = format_ipv4(&addr, ip_out, ip_out_len);
    freeaddrinfo(res);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "DNS %s -> %s", hostname, ip_out);
    return ESP_OK;
}

esp_err_t dbmap_resolve_ipv4(const char *hostname, char *ip_out, size_t ip_out_len)
{
    if (hostname == NULL || hostname[0] == '\0' || ip_out == NULL || ip_out_len < 8) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(hostname);
    if (is_local_hostname(hostname, len)) {
        esp_err_t err = resolve_mdns_ipv4(hostname, ip_out, ip_out_len);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        /* Fall through to lwIP getaddrinfo, which can also send one-shot mDNS. */
    }
    return resolve_dns_ipv4(hostname, ip_out, ip_out_len);
}
