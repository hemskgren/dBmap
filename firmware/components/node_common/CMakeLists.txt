idf_component_register(
    SRCS
        "identity.c"
        "nvs_store.c"
        "net_wifi.c"
        "hub_provision.c"
        "hub_mqtt.c"
        "health.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash mqtt esp_wifi esp_event esp_netif esp_http_client json esp_timer
    EMBED_TXTFILES "${PROJECT_DIR}/main/root_ca.pem"
)
