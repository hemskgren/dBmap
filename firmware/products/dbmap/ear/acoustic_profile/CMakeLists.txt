idf_component_register(
    SRCS
        "as_profile_as001.c"
        "as_profile_registry.c"
        "as_profile_match.c"
    INCLUDE_DIRS "include"
    REQUIRES
        audio_engine
)
