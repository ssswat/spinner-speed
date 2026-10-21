cmake_minimum_required(VERSION 3.26)
project(spinner_speed CXX)

if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "v2.0.3")
endif ()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mod.json.in"
               "${CMAKE_CURRENT_BINARY_DIR}/mod.json" @ONLY)

add_mod(spinner_speed
        FEATURES game
        SOURCES src/mod.cpp
        MOD_JSON "${CMAKE_CURRENT_BINARY_DIR}/mod.json"
)
