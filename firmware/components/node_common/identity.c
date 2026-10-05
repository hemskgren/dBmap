#include "dbmap_types.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

void dbmap_effective_defaults(dbmap_effective_t *out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->identity.hardware_revision, CONFIG_DBMAP_HARDWARE_REVISION, sizeof(out->identity.hardware_revision) - 1);
    strncpy(out->identity.provisioning_state, "pending", sizeof(out->identity.provisioning_state) - 1);
    strncpy(out->network.ssid, CONFIG_DBMAP_WIFI_SSID, sizeof(out->network.ssid) - 1);
    strncpy(out->network.password, CONFIG_DBMAP_WIFI_PASSWORD, sizeof(out->network.password) - 1);
    strncpy(out->network.hub_http_url, CONFIG_DBMAP_HUB_HTTP_URL, sizeof(out->network.hub_http_url) - 1);
    out->network.mqtt_port = 8883;
    out->network.mqtt_use_tls = true;
    strncpy(out->bootstrap_token, CONFIG_DBMAP_BOOTSTRAP_TOKEN, sizeof(out->bootstrap_token) - 1);
    out->provisioned = false;
    dbmap_rebuild_topics(out);
}

void dbmap_rebuild_topics(dbmap_effective_t *cfg)
{
    const char *kind = "esp-output";
    char slug[DBMAP_NODE_ID_MAX];
    if (cfg->identity.node_id[0] == '\0') {
        snprintf(slug, sizeof(slug), "unprovisioned");
    } else {
        strncpy(slug, cfg->identity.node_id, sizeof(slug) - 1);
        for (char *p = slug; *p; ++p) {
            if (*p >= 'A' && *p <= 'Z') {
                *p = (char)(*p - 'A' + 'a');
            }
        }
    }
    snprintf(cfg->topics.hello, sizeof(cfg->topics.hello), "%s/local/%s/hello", kind, slug);
    snprintf(cfg->topics.keepalive, sizeof(cfg->topics.keepalive), "%s/local/%s/keepalive", kind, slug);
    snprintf(cfg->topics.health, sizeof(cfg->topics.health), "%s/local/%s/health", kind, slug);
    snprintf(cfg->topics.status, sizeof(cfg->topics.status), "%s/local/%s/status", kind, slug);
    snprintf(cfg->topics.command, sizeof(cfg->topics.command), "%s/local/%s/command", kind, slug);
    snprintf(cfg->topics.ack, sizeof(cfg->topics.ack), "%s/local/%s/ack", kind, slug);
    snprintf(cfg->topics.config, sizeof(cfg->topics.config), "%s/local/%s/config", kind, slug);
}
