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

/**
 * Stage 3: post a Wi-Fi stop/start to sp_net (must be responsive).
 * Does not re-init the driver or re-register event handlers.
 */
void sp_wifi_request_iface_restart(uint32_t iface_generation);

/** Current interface generation (bumped for each Stage 3 begin). */
uint32_t sp_wifi_iface_generation(void);

typedef void (*sp_wifi_stage3_event_fn)(void);

/** Optional Stage 3 lifecycle callbacks (connectivity supervisor). */
void sp_wifi_set_stage3_hooks(
    sp_wifi_stage3_event_fn on_stop_ok,
    sp_wifi_stage3_event_fn on_stop_fail,
    sp_wifi_stage3_event_fn on_start_ok,
    sp_wifi_stage3_event_fn on_start_fail,
    sp_wifi_stage3_event_fn on_connect_started,
    void (*on_got_ip)(uint32_t iface_generation)
);
