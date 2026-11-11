cmake_minimum_required(VERSION 3.16)
# Platform capture/observation stack + dBmap Ear product modules.
# List platform components explicitly (same tree as Output uses for node_common
# alone). Do not include output_components (Helix/LittleFS) — builder has no net.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../platform/node_common"
    "${CMAKE_CURRENT_LIST_DIR}/../platform/mic"
    "${CMAKE_CURRENT_LIST_DIR}/../platform/observation"
    "${CMAKE_CURRENT_LIST_DIR}/../products/dbmap/ear"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-ear)
