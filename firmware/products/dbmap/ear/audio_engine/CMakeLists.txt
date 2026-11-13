idf_component_register(
    SRCS
        "ae_signature.c"
        "ae_history.c"
        "ae_engine.c"
        "ae_align.c"
        "ae_gap.c"
        "ae_stereo.c"
        "ae_time.c"
    INCLUDE_DIRS "include"
    REQUIRES
        ear_acoustic
)
