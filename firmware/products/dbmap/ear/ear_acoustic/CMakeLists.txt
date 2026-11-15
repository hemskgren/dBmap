# Compile only the acoustic analysis modules selected by Kconfig.
# Dependency chain (Kconfig): L1 → L0+OBS_L0; L2 → L1. MIC_L0 retains PCM / window L0.
set(srcs "ear_acoustic_runtime.c")

if(CONFIG_DBMAP_EAR_MIC_L0)
    list(APPEND srcs "acoustic_l0.c")
endif()

if(CONFIG_DBMAP_EAR_OBS_L1)
    list(APPEND srcs "acoustic_l1.c")
endif()

if(CONFIG_DBMAP_EAR_OBS_L2)
    list(APPEND srcs "acoustic_l2.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES
        mic
        esp_timer
        log
        freertos
)
