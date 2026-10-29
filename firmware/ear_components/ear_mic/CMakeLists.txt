idf_component_register(
    SRCS
        "audio_metrics.c"
        "mic_model.c"
        "ear_mic.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        node_common
        driver
        esp_driver_i2s
        esp_timer
        freertos
        json
        log
)
