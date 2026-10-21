cmake_minimum_required(VERSION 3.26)
project(spinner_speed CXX)

if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "v2.0.3")
endif ()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

# O mod.json e gerado aqui (sempre com quebras de linha LF) para ficar
# byte a byte identico em todas as plataformas, senao o "Combine bundles" falha.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mod.json" [=[{
  "id": "dev.spinner_speed",
  "name": "Spinner Speed",
  "version": "1.0.0",
  "author": "Spinner Speed",
  "description": "O Spinner acelera progressivamente enquanto R (giro das serras) esta pressionado, ate um limite maximo."
}
]=])

add_mod(spinner_speed
        FEATURES game
        SOURCES src/mod.cpp
        MOD_JSON "${CMAKE_CURRENT_BINARY_DIR}/mod.json"
)
