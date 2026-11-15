# EAR acoustic runtime — product integration over Platform DSP (sp_acoustic_*).
# L0/L1/L2 algorithm sources live in firmware/platform/audio.

idf_component_register(
    SRCS "ear_acoustic_runtime.c"
    INCLUDE_DIRS "include"
    REQUIRES
        audio
        mic
        esp_timer
        log
        freertos
)
