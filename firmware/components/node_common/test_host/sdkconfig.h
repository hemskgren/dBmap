/* Stub for host builds of identity/backoff (ESP-IDF provides the real header). */
#pragma once

#ifndef CONFIG_DBMAP_HARDWARE_REVISION
#define CONFIG_DBMAP_HARDWARE_REVISION "generic"
#endif
#ifndef CONFIG_DBMAP_WIFI_SSID
#define CONFIG_DBMAP_WIFI_SSID ""
#endif
#ifndef CONFIG_DBMAP_WIFI_PASSWORD
#define CONFIG_DBMAP_WIFI_PASSWORD ""
#endif
#ifndef CONFIG_DBMAP_HUB_HTTP_URL
#define CONFIG_DBMAP_HUB_HTTP_URL "https://hub.local:8443"
#endif
#ifndef CONFIG_DBMAP_BOOTSTRAP_TOKEN
#define CONFIG_DBMAP_BOOTSTRAP_TOKEN ""
#endif
#ifndef CONFIG_DBMAP_NODE_TYPE_EAR
#define CONFIG_DBMAP_NODE_TYPE_EAR 0
#endif
#ifndef CONFIG_DBMAP_NODE_TYPE_OUTPUT
#define CONFIG_DBMAP_NODE_TYPE_OUTPUT (!CONFIG_DBMAP_NODE_TYPE_EAR)
#endif
