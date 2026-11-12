# Authoritative SP SemVer: firmware/platform/VERSION (single source with builder).
set(SP_VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/../VERSION")
if(NOT EXISTS "${SP_VERSION_FILE}")
    message(FATAL_ERROR "Missing Sensor Platform VERSION file: ${SP_VERSION_FILE}")
endif()
file(READ "${SP_VERSION_FILE}" _SP_VER_RAW)
string(STRIP "${_SP_VER_RAW}" SP_VERSION_STRING)
if(SP_VERSION_STRING STREQUAL "")
    message(FATAL_ERROR "firmware/platform/VERSION is empty")
endif()
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/sp_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/sp_version_gen.h"
    @ONLY
)

idf_component_register(
    SRCS
        "identity.c"
        "mic_count.c"
        "nvs_store.c"
        "serial_config.c"
        "net_wifi.c"
        "net_resolve.c"
        "net_i2s.c"
        "time_sync.c"
        "hub_http.c"
        "hub_provision.c"
        "hub_desired.c"
        "hub_ota.c"
        "hub_mqtt.c"
        "health.c"
        "diag.c"
        "sp_version.c"
        "backoff.c"
        "lifecycle.c"
        "wifi_reconnect_sm.c"
    INCLUDE_DIRS "include" "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES nvs_flash mqtt esp_wifi esp_event esp_netif esp_http_client json esp_timer vfs esp_driver_uart esp_driver_i2s mdns freertos app_update mbedtls esp_app_format esp_system heap
    EMBED_TXTFILES "${PROJECT_DIR}/main/root_ca.pem"
)
