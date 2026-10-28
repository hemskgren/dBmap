idf_component_register(
    SRCS
        "identity.c"
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
        "backoff.c"
        "lifecycle.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash mqtt esp_wifi esp_event esp_netif esp_http_client json esp_timer vfs esp_driver_uart esp_driver_i2s mdns freertos app_update mbedtls esp_app_format esp_system
    EMBED_TXTFILES "${PROJECT_DIR}/main/root_ca.pem"
)
