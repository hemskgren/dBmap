idf_component_register(
    SRCS
        "acoustic_l0.c"
        "acoustic_l1.c"
        "acoustic_l2.c"
        "audio_metrics.c"
        "background_rms.c"
        "i2s_stereo.c"
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
        heap
        json
        log
)
