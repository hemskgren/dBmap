#include "dbmap_health.h"

#include <stdlib.h>

#include "cJSON.h"
#include "dbmap_mqtt.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static char *build_keepalive(const dbmap_effective_t *cfg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "node_id", cfg->identity.node_id[0] ? cfg->identity.node_id : "UNPROVISIONED");
    cJSON_AddStringToObject(o, "firmware_version", CONFIG_DBMAP_FIRMWARE_VERSION);
    cJSON_AddStringToObject(o, "hardware_revision", cfg->identity.hardware_revision);
    cJSON_AddNumberToObject(o, "config_version", cfg->config.config_version);
    cJSON_AddNumberToObject(o, "calibration_version", cfg->config.calibration_version);
    cJSON_AddStringToObject(o, "classifier_version", cfg->config.classifier_version);
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000ULL));
    cJSON_AddBoolToObject(o, "pending_configuration_change", false);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

esp_err_t dbmap_publish_hello(const dbmap_effective_t *cfg)
{
    char *json = build_keepalive(cfg);
    esp_err_t err = dbmap_mqtt_publish_json(cfg->topics.hello, json, 1);
    free(json);
    return err;
}

esp_err_t dbmap_publish_health(const dbmap_effective_t *cfg)
{
    char *json = build_keepalive(cfg);
    esp_err_t err = dbmap_mqtt_publish_json(cfg->topics.health, json, 0);
    dbmap_mqtt_publish_json(cfg->topics.keepalive, json, 0);
    free(json);
    return err;
}

esp_err_t dbmap_publish_status(const dbmap_effective_t *cfg, const char *status_json)
{
    return dbmap_mqtt_publish_json(cfg->topics.status, status_json, 1);
}
