# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

# Alle Quell-Dateien sammeln, aber Initial-Boardtester.cpp und Stub-Dateien ausschließen
FILE(GLOB_RECURSE app_sources 
    ${CMAKE_SOURCE_DIR}/src/*.cpp
    ${CMAKE_SOURCE_DIR}/src/*.c
)

# Initial-Boardtester.cpp und Stub-Dateien aus Liste entfernen
list(FILTER app_sources EXCLUDE REGEX ".*Initial-Boardtester\\.cpp.*")
list(FILTER app_sources EXCLUDE REGEX ".*esp_lp_core_stub\\.cpp.*")

idf_component_register(SRCS ${app_sources})
