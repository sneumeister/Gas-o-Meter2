cmake_minimum_required(VERSION 3.16.0)

# Component Manager aktivieren (für mDNS-Komponente in ESP-IDF 5.x)
# mDNS ist ab ESP-IDF 5.0 eine externe Komponente und muss über Component Manager hinzugefügt werden
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Projekt-Name und Version
# Versions-Informationen manuell setzen, um Git-Warnung zu vermeiden
# ESP-IDF versucht automatisch Git-Version zu ermitteln, aber das kann in PlatformIO fehlschlagen
project(gas-o-meter2 VERSION 0.5)
