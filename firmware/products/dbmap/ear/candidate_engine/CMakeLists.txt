idf_component_register(
    SRCS
        "ce_decision.c"
        "ce_module.c"
        "ce_workflow.c"
        "ce_manager.c"
        "ce_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES
        audio_engine
        log
)
