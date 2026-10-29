idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES node_common ear_mic nvs_flash mqtt esp_wifi esp_event esp_netif driver json
)
