#pragma once

#include "dbmap_types.h"
#include "esp_err.h"
#include "mqtt_client.h"

typedef void (*dbmap_command_cb_t)(const char *payload, int len, void *ctx);

esp_err_t dbmap_mqtt_start(const dbmap_effective_t *cfg, dbmap_command_cb_t on_command, void *ctx);
esp_mqtt_client_handle_t dbmap_mqtt_handle(void);
esp_err_t dbmap_mqtt_publish_json(const char *topic, const char *json, int qos);
