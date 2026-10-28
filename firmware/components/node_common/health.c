#include "dbmap_health.h"

#include <stdlib.h>

#include "cJSON.h"
#include "dbmap_diag.h"
#include "dbmap_mqtt.h"
#include "dbmap_wifi.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static cJSON *build_state(const dbmap_effective_t *cfg, bool include_diag)
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
    if (dbmap_wifi_get_rssi(&rssi_dbm) == ESP_OK) {
        cJSON_AddNumberToObject(o, "wifi_rssi_dbm", rssi_dbm);
    }
    if (include_diag) {
        dbmap_diag_attach_json(o);
    }
    return o;
}

esp_err_t dbmap_publish_hello(const dbmap_effective_t *cfg)
{
    /* Hello stays lean: identity/state only (no rolling diag counters). */
    cJSON *o = build_state(cfg, false);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    esp_err_t err = dbmap_mqtt_publish_json(cfg->topics.hello, json, 1);
    free(json);
    return err;
}

esp_err_t dbmap_publish_health(const dbmap_effective_t *cfg)
{
    cJSON *o = build_state(cfg, true);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    esp_err_t err = dbmap_mqtt_publish_json(cfg->topics.health, json, 0);
    if (err == ESP_OK) {
        /* Same payload on keepalive; clear oneshots only after a successful publish. */
        (void)dbmap_mqtt_publish_json(cfg->topics.keepalive, json, 0);
        dbmap_diag_clear_oneshots();
    } else {
        /* Keep counters/oneshots if MQTT is down; send on the next successful publish. */
        (void)dbmap_mqtt_publish_json(cfg->topics.keepalive, json, 0);
    }
    free(json);
    return err;
}

esp_err_t dbmap_publish_status(const dbmap_effective_t *cfg, const char *status_json)
{
    return dbmap_mqtt_publish_json(cfg->topics.status, status_json, 1);
}
