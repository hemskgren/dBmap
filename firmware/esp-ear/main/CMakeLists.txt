idf_component_register(
    SRCS "app_main.c" "ear_boot_order.c"
    INCLUDE_DIRS "."
    REQUIRES node_common ear_mic ear_observation nvs_flash mqtt esp_wifi esp_event esp_netif driver json
)
