#include "sp_mdns_policy.h"

#include <strings.h>
#include <string.h>

bool sp_hostname_ends_with_local(const char *hostname)
{
    if (hostname == NULL || hostname[0] == '\0') {
        return false;
    }
    size_t len = strlen(hostname);
    return len > 6 && strcasecmp(hostname + len - 6, ".local") == 0;
}

bool sp_https_url_host_ends_with_local(const char *hub_http_url)
{
    if (hub_http_url == NULL || strncmp(hub_http_url, "https://", 8) != 0) {
        return false;
    }
    const char *host_start = hub_http_url + 8;
    const char *host_end = strpbrk(host_start, ":/");
    size_t host_len = host_end ? (size_t)(host_end - host_start) : strlen(host_start);
    if (host_len == 0 || host_len >= 128) {
        return false;
    }
    char hostname[128];
    memcpy(hostname, host_start, host_len);
    hostname[host_len] = '\0';
    return sp_hostname_ends_with_local(hostname);
}

bool sp_network_needs_mdns(const sp_network_t *net)
{
    if (net == NULL) {
        return true;
    }

    /* Local / missing / unknown → keep historical always-on mDNS. */
    if (net->mqtt_profile[0] == '\0' || strcmp(net->mqtt_profile, "external") != 0) {
        return true;
    }

    /* External: mDNS only when a configured name actually needs .local. */
    if (net->mqtt_host[0] == '\0') {
        /* hub_mqtt.c defaults empty mqtt_host to "hub.local". */
        return true;
    }
    if (sp_hostname_ends_with_local(net->mqtt_host)) {
        return true;
    }
    if (sp_https_url_host_ends_with_local(net->hub_http_url)) {
        return true;
    }
    return false;
}
