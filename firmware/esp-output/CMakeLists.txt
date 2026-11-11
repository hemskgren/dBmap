cmake_minimum_required(VERSION 3.16)
# Platform: node_common only. Do not point at ../platform (that also discovers
# mic + observation, which need Ear Kconfig and must not build into Output).
# Output-only: output_audio (Helix/LittleFS) under output_components.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../platform/node_common"
    "${CMAKE_CURRENT_LIST_DIR}/../output_components"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-output)
