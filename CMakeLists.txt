cmake_minimum_required(VERSION 3.16.0)

# Projekt-Version setzen VOR include(project.cmake) (verhindert Git-Abfrage-Fehler)
# ESP-IDF versucht sonst, Git-Informationen zu lesen, auch wenn kein Git-Repo vorhanden ist
set(PROJECT_VER "0.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Gas-o-Meter2 VERSION ${PROJECT_VER})
