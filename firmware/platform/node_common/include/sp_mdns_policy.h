#pragma once

#include <stdbool.h>

#include "sp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** True when hostname ends with ".local" (case-insensitive). */
bool sp_hostname_ends_with_local(const char *hostname);

/**
 * True when an https:// Hub URL's host component ends with ".local".
 * Non-HTTPS or unparseable URLs return false.
 */
bool sp_https_url_host_ends_with_local(const char *hub_http_url);

/**
 * Whether Wi-Fi bring-up should start mDNS for this network config.
 *
 * - mqtt_profile "external": only if mqtt_host or Hub HTTPS host is .local,
 *   or mqtt_host is empty (MQTT defaults to hub.local).
 * - mqtt_profile "local": always true (LAN Hub discovery).
 * - missing/unknown profile: true (safe existing behavior).
 */
bool sp_network_needs_mdns(const sp_network_t *net);

#ifdef __cplusplus
}
#endif
