#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sp_types.h"
#include "esp_err.h"
#include "mqtt_client.h"

typedef void (*sp_command_cb_t)(const char *payload, int len, void *ctx);

esp_err_t sp_mqtt_start(const sp_effective_t *cfg, sp_command_cb_t on_command, void *ctx);
esp_mqtt_client_handle_t sp_mqtt_handle(void);
bool sp_mqtt_is_connected(void);
/** Ensure a reconnect attempt is scheduled when MQTT is down (safe to call often). */
void sp_mqtt_request_reconnect(void);
esp_err_t sp_mqtt_publish_json(const char *topic, const char *json, int qos);

/**
 * Publish JSON and wait for MQTT QoS PUBACK (MQTT_EVENT_PUBLISHED) up to timeout_ms.
 * Never waits forever. Returns ESP_OK on puback, ESP_ERR_TIMEOUT if none,
 * ESP_ERR_INVALID_STATE if MQTT is down.
 */
esp_err_t sp_mqtt_publish_json_wait(
    const char *topic,
    const char *json,
    int qos,
    uint32_t timeout_ms
);

/**
 * Cancel reconnect scheduling and optionally request a quick disconnect.
 * Used after OTA install report when reboot is imminent. Bounded; never blocks
 * reboot indefinitely (caller enforces the overall deadline).
 */
void sp_mqtt_prepare_ota_reboot(void);
