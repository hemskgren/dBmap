#pragma once

#include <stdbool.h>

#include "sp_types.h"
#include "esp_err.h"

esp_err_t sp_wifi_start(const sp_network_t *net);
esp_err_t sp_wifi_get_rssi(int *rssi_dbm);
/** True when STA has an IPv4 address (GOT_IP). Used to gate MQTT reconnect. */
bool sp_wifi_has_ip(void);
/** Cancel Wi-Fi reconnect scheduling before an imminent OTA reboot. */
void sp_wifi_prepare_ota_reboot(void);
