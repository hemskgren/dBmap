idf_component_register(
    SRCS
        "ae_device.c"
    INCLUDE_DIRS "include"
    REQUIRES
        audio_engine
        ear_acoustic
        mic
        log
)
