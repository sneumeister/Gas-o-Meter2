cmake_minimum_required(VERSION 3.16.0)

# explizit den Git-Arbeitsordner setzen
set(GIT_DIR "${PROJECT_DIR}")
set(PROJECT_VER "0.0.1")
set(IDF_PROJECT_VER "0.0.1")
set(IDF_VER_IGNORE_GIT ON)
set(SUBMODULES_IGNORE_GIT ON)

# 1. Pfad zur version.h
set(VERSION_H_PATH "${CMAKE_SOURCE_DIR}/include/version.h")

# Fallbacks
set(extracted_name "gas-o-meter-default")
set(extracted_version "0.0.1")

# 2. Datei als EINEN String einlesen (vermeidet Listen-Probleme)
if(EXISTS "${VERSION_H_PATH}")
    file(READ "${VERSION_H_PATH}" VERSION_CONTENT)

    # Regex Erklärung:
    # .*?           -> Finde alles (nicht gierig) bis zum Treffer
    # \\[\\]        -> Findet die eckigen Klammern []
    # \"([^\"]+)\"  -> Findet den Text zwischen den Anführungszeichen (Gruppe 1)
    
    # Extraktion Name
    if(VERSION_CONTENT MATCHES "PROJECT_NAME\\[\\] = \"([^\"]+)\"")
        set(extracted_name "${CMAKE_MATCH_1}")
    endif()

    # Extraktion Version
    if(VERSION_CONTENT MATCHES "PROJECT_VERSION\\[\\] = \"([0-9.]+)\"")
        set(extracted_version "${CMAKE_MATCH_1}")
    endif()
else()
    message(WARNING "version.h nicht gefunden!")
endif()

# 3. ESP-IDF Konfiguration ...
set(IDF_PROJECT_VER "${extracted_version}")
set(PROJECT_VER "${extracted_version}")

# 4. Info-Ausgabe (Schön formatiert)
message(NOTICE " ")
message(NOTICE "========================================================")
message(NOTICE "  GAS-O-METER BUILD SYSTEM 2026")
message(NOTICE "  ------------------------------------------------------")
message(NOTICE "  PROJEKT:  ${extracted_name}")
message(NOTICE "  VERSION:  ${extracted_version}")
message(NOTICE "  PFAD:     ${VERSION_H_PATH}")
message(NOTICE "========================================================")
message(NOTICE " ")

# 5. ESP-IDF Framework laden
message(NOTICE "==> include($ENV{IDF_PATH}/tools/cmake/project.cmake)")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Für Littlefs gemaess https://github.com/joltwallet/esp_littlefs
get_filename_component(configName "${CMAKE_BINARY_DIR}" NAME)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/.pio/libdeps/${configName}/esp_littlefs")
message(NOTICE "==> LittelFS ${CMAKE_SOURCE_DIR}/.pio/libdeps/${configName}/esp_littlefs")

# 6. Projekt definieren
project(${extracted_name} VERSION "${extracted_version}")
