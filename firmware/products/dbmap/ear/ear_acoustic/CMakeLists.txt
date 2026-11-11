idf_component_register(
    SRCS
        "acoustic_l0.c"
        "acoustic_l1.c"
        "acoustic_l2.c"
        "ear_acoustic_runtime.c"
    INCLUDE_DIRS "include"
    REQUIRES
        mic
        esp_timer
        log
        freertos
)
