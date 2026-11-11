#include "sp_health.h"

#include <stddef.h>
#include <stdlib.h>

#include "sp_diag.h"
#include "sp_mqtt.h"
#include "sp_wifi.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#define SP_HEALTH_MAX_ENRICHERS 4

static sp_health_enrich_fn s_enrichers[SP_HEALTH_MAX_ENRICHERS];
static size_t s_enricher_count;

void sp_health_add_enricher(sp_health_enrich_fn fn)
{
    if (fn == NULL || s_enricher_count >= SP_HEALTH_MAX_ENRICHERS) {
        return;
    }
    s_enrichers[s_enricher_count++] = fn;
}

void sp_health_set_enricher(sp_health_enrich_fn fn)
{
    s_enricher_count = 0;
    if (fn != NULL) {
        sp_health_add_enricher(fn);
    }
}

static cJSON *build_state(const sp_effective_t *cfg, bool include_diag)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "protocol_version", 1);
    cJSON_AddStringToObject(o, "node_id", cfg->identity.node_id[0] ? cfg->identity.node_id : "UNPROVISIONED");
    cJSON_AddStringToObject(o, "firmware_version", CONFIG_DBMAP_FIRMWARE_VERSION);
    cJSON_AddStringToObject(o, "hardware_revision", cfg->identity.hardware_revision);
    cJSON_AddNumberToObject(o, "config_version", cfg->config.config_version);
    cJSON_AddNumberToObject(o, "calibration_version", cfg->config.calibration_version);
    cJSON_AddStringToObject(o, "classifier_version", cfg->config.classifier_version);
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000ULL));
    int rssi_dbm;
    if (sp_wifi_get_rssi(&rssi_dbm) == ESP_OK) {
        cJSON_AddNumberToObject(o, "wifi_rssi_dbm", rssi_dbm);
    }
    if (include_diag) {
        sp_diag_attach_json(o);
    }
    for (size_t i = 0; i < s_enricher_count; ++i) {
        if (s_enrichers[i] != NULL) {
            s_enrichers[i](o, cfg, include_diag);
        }
    }
    return o;
}

esp_err_t sp_publish_hello(const sp_effective_t *cfg)
{
    /* Hello stays lean: identity/state only (no rolling diag counters). */
    cJSON *o = build_state(cfg, false);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    esp_err_t err = sp_mqtt_publish_json(cfg->topics.hello, json, 1);
    free(json);
    return err;
}

esp_err_t sp_publish_health(const sp_effective_t *cfg)
{
    cJSON *o = build_state(cfg, true);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    esp_err_t err = sp_mqtt_publish_json(cfg->topics.health, json, 0);
    if (err == ESP_OK) {
        /* Same payload on keepalive; clear oneshots only after a successful publish. */
        (void)sp_mqtt_publish_json(cfg->topics.keepalive, json, 0);
        sp_diag_clear_oneshots();
    } else {
        /* Keep counters/oneshots if MQTT is down; send on the next successful publish. */
        (void)sp_mqtt_publish_json(cfg->topics.keepalive, json, 0);
    }
    free(json);
    return err;
}

esp_err_t sp_publish_status(const sp_effective_t *cfg, const char *status_json)
{
    return sp_mqtt_publish_json(cfg->topics.status, status_json, 1);
}
