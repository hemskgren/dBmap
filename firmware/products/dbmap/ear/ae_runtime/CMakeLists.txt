idf_component_register(
    SRCS
        "ae_device.c"
        "ae_sig_capture.c"
    INCLUDE_DIRS "include"
    REQUIRES
        audio_engine
        ear_acoustic
        acoustic_profile
        audio
        mic
        log
)
