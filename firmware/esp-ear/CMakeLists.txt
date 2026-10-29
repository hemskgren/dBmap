cmake_minimum_required(VERSION 3.16)
# Shared node_common + Ear-only ear_mic. Do not include output_components
# (Helix/LittleFS registry deps) — the builder has no internet.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components"
    "${CMAKE_CURRENT_LIST_DIR}/../ear_components"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-ear)
