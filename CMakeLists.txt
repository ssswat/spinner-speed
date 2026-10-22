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
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mod.json" [=[{"id": "dev.spinner_speed", "name": "Spinner Speed", "version": "1.0.0", "author": "Spinner Speed", "description": "Cada ataque de serra do Spinner soma velocidade, ate um maximo ajustavel no jogo."}]=])

add_mod(spinner_speed
        FEATURES game
        SOURCES src/mod.cpp
        MOD_JSON "${CMAKE_CURRENT_BINARY_DIR}/mod.json"
        RES_DIR res
)
