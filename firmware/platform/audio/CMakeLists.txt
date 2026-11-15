# Sensor Platform acoustic DSP (L0 / L1 / L2) + multi-mic analyze.
# Product-independent. EAR and other products select layers via CONFIG_SP_AUDIO_L*.

set(srcs "")

if(CONFIG_SP_AUDIO_L0)
    list(APPEND srcs "sp_acoustic_l0.c")
    list(APPEND srcs "sp_audio_analyze.c")
endif()

if(CONFIG_SP_AUDIO_L1)
    list(APPEND srcs "sp_acoustic_l1.c")
endif()

if(CONFIG_SP_AUDIO_L2)
    list(APPEND srcs "sp_acoustic_l2.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES
        esp_timer
)
