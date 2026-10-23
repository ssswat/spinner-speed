cmake_minimum_required(VERSION 3.26)
project(spinner_speed CXX)

if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "v2.0.3")
endif ()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

# O mod.json e gerado aqui em UMA UNICA LINHA (sem quebras de linha), para ficar
# byte a byte identico em todas as plataformas. No Windows o arquivo do repositorio
# pode ter quebras CRLF, e isso fazia o "Combine bundles" falhar.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mod.json" [=[{"id": "dev.spinner_speed", "name": "Spinner Speed", "version": "1.6.0", "author": "Spinner Speed", "description": "Segurando A em cima do Spinner, ele acelera aos poucos ate um maximo ajustavel, vira mais rapido e o Link encara a direcao. A animacao das serras fica em loop."}]=])

add_mod(spinner_speed
        FEATURES game
        SOURCES src/mod.cpp
        MOD_JSON "${CMAKE_CURRENT_BINARY_DIR}/mod.json"
        RES_DIR res
)
