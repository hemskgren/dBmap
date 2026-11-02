idf_component_register(
    SRCS
        "observation_payload.c"
        "observation_seq.c"
        "activity_detector.c"
        "ear_observation.c"
    INCLUDE_DIRS "include"
    REQUIRES
        node_common
        ear_mic
        json
        nvs_flash
        esp_timer
        log
        esp_system
        freertos
)
