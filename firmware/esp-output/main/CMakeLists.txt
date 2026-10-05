idf_component_register(
    SRCS "app_main.c" "output_command.c"
    INCLUDE_DIRS "."
    REQUIRES node_common nvs_flash mqtt esp_wifi esp_event esp_netif driver json
)
