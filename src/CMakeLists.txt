# ESP-IDF Component für gas-o-meter2
# Nur main_idf.cpp kompilieren (main_arduino.cpp bleibt als Referenz)

get_filename_component(INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include" ABSOLUTE)

# ArduinoJson Bibliothek einbinden (über PlatformIO lib_deps)
# PlatformIO bindet Bibliotheken automatisch ein, wenn lib_compat_mode = off gesetzt ist
# Die Include-Pfade werden automatisch von PlatformIO gesetzt
# Falls nötig, können wir hier zusätzliche Include-Pfade hinzufügen:
# get_filename_component(ARDUINOJSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.pio/libdeps/espidf/ArduinoJson/src" ABSOLUTE)
# Aber normalerweise nicht nötig, da PlatformIO das automatisch macht

idf_component_register(
    SRCS "main_idf.cpp"
    INCLUDE_DIRS "${INCLUDE_DIR}"
    REQUIRES 
        mdns                    # Externe Komponente via Component Manager (idf_component.yml)
        esp_http_server
        joltwallet__littlefs    # Externe Komponente via Component Manager (idf_component.yml)
        esp_adc
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        driver
        freertos
)

# Compiler-Optionen setzen (nach idf_component_register)
# BUILD_TIMESTAMP wird jetzt in version.h aus __DATE__ und __TIME__ berechnet
target_compile_options(${COMPONENT_LIB} PRIVATE
    "-DTEMPLATE_PLACEHOLDER=96"
    "-DARDUINOJSON_ENABLE_PROGMEM=0"
)
