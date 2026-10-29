cmake_minimum_required(VERSION 3.16)
# Shared node_common + Output-only output_audio (Helix/LittleFS).
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components"
    "${CMAKE_CURRENT_LIST_DIR}/../output_components"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-output)
