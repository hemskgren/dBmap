idf_component_register(
    SRCS
        "episode_registry.c"
        "observation_lifecycle.c"
        "observation_seq.c"
    INCLUDE_DIRS "include"
    REQUIRES log
)
