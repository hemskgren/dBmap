idf_component_register(
    SRCS
        "sig_engine.c"
        "ear_siren_se_public.c"
    INCLUDE_DIRS "include"
    REQUIRES
        audio_engine
        acoustic_profile
    PRIV_REQUIRES
        freertos
)
