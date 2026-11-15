/**
 * Host tests: conditional mDNS policy (no ESP-IDF mdns stack).
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g \
 *     -I firmware/platform/node_common/include \
 *     firmware/platform/node_common/net_mdns_policy.c \
 *     firmware/platform/node_common/test_host/test_mdns_policy.c \
 *     -o /tmp/test_mdns_policy && /tmp/test_mdns_policy
 */
#include "sp_mdns_policy.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void fill_net(sp_network_t *net, const char *profile, const char *mqtt_host, const char *hub_url)
{
    memset(net, 0, sizeof(*net));
    if (profile != NULL) {
        strncpy(net->mqtt_profile, profile, sizeof(net->mqtt_profile) - 1);
    }
    if (mqtt_host != NULL) {
        strncpy(net->mqtt_host, mqtt_host, sizeof(net->mqtt_host) - 1);
    }
    if (hub_url != NULL) {
        strncpy(net->hub_http_url, hub_url, sizeof(net->hub_http_url) - 1);
    }
}

static void test_hostname_helper(void)
{
    EXPECT(sp_hostname_ends_with_local("hub.local"));
    EXPECT(sp_hostname_ends_with_local("HUB.LOCAL"));
    EXPECT(!sp_hostname_ends_with_local("dbmap.ddns.net"));
    EXPECT(!sp_hostname_ends_with_local("192.168.1.10"));
    EXPECT(!sp_hostname_ends_with_local(""));
    EXPECT(!sp_hostname_ends_with_local(NULL));
    EXPECT(sp_https_url_host_ends_with_local("https://hub.local:8443"));
    EXPECT(sp_https_url_host_ends_with_local("https://hub.local/"));
    EXPECT(!sp_https_url_host_ends_with_local("https://dbmap.ddns.net:8443"));
    EXPECT(!sp_https_url_host_ends_with_local("http://hub.local"));
}

static void test_external_public_dns(void)
{
    sp_network_t net;
    fill_net(&net, "external", "dbmap.ddns.net", "https://dbmap.ddns.net:8443");
    EXPECT(!sp_network_needs_mdns(&net));
}

static void test_external_ip(void)
{
    sp_network_t net;
    fill_net(&net, "external", "81.233.16.197", "https://81.233.16.197:8443");
    EXPECT(!sp_network_needs_mdns(&net));
}

static void test_external_mqtt_local(void)
{
    sp_network_t net;
    fill_net(&net, "external", "hub.local", "https://dbmap.ddns.net:8443");
    EXPECT(sp_network_needs_mdns(&net));
}

static void test_external_hub_local(void)
{
    sp_network_t net;
    fill_net(&net, "external", "dbmap.ddns.net", "https://hub.local:8443");
    EXPECT(sp_network_needs_mdns(&net));
}

static void test_external_empty_mqtt_host(void)
{
    sp_network_t net;
    fill_net(&net, "external", "", "https://dbmap.ddns.net:8443");
    /* Empty mqtt_host defaults to hub.local in MQTT start. */
    EXPECT(sp_network_needs_mdns(&net));
}

static void test_local_profile(void)
{
    sp_network_t net;
    fill_net(&net, "local", "hub.local", "https://hub.local:8443");
    EXPECT(sp_network_needs_mdns(&net));

    fill_net(&net, "local", "dbmap.ddns.net", "https://dbmap.ddns.net:8443");
    EXPECT(sp_network_needs_mdns(&net));
}

static void test_missing_unknown_profile(void)
{
    sp_network_t net;
    fill_net(&net, "", "dbmap.ddns.net", "https://dbmap.ddns.net:8443");
    EXPECT(sp_network_needs_mdns(&net));

    fill_net(&net, "weird", "dbmap.ddns.net", "https://dbmap.ddns.net:8443");
    EXPECT(sp_network_needs_mdns(&net));

    EXPECT(sp_network_needs_mdns(NULL));
}

static void test_dns_names_not_forced_local(void)
{
    /* Policy must not classify ordinary DNS as needing mDNS. */
    EXPECT(!sp_hostname_ends_with_local("example.com"));
    EXPECT(!sp_hostname_ends_with_local("localhost")); /* no .local suffix */
    sp_network_t net;
    fill_net(&net, "external", "example.com", "https://example.com");
    EXPECT(!sp_network_needs_mdns(&net));
}

int main(void)
{
    test_hostname_helper();
    test_external_public_dns();
    test_external_ip();
    test_external_mqtt_local();
    test_external_hub_local();
    test_external_empty_mqtt_host();
    test_local_profile();
    test_missing_unknown_profile();
    test_dns_names_not_forced_local();

    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
