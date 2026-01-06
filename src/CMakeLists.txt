# ESP-IDF Component für gas-o-meter2
# Nur main_idf.cpp kompilieren (main_arduino.cpp bleibt als Referenz)

# PROJECT_DIR ist die ESP-IDF Variable für das Projekt-Root-Verzeichnis
get_filename_component(INCLUDE_DIR "${PROJECT_DIR}/include" ABSOLUTE)

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
        ulp                     # ULP-Header für ulp_lp_core.h APIs
)

# LP-Core Binary einbetten
# WICHTIG: ulp_app_name sollte NICHT mit "ulp_" beginnen, da ulp_embed_binary() automatisch "ulp_" als Präfix hinzufügt!
# Mit "lp_core" wird Verzeichnis "ulp_lp_core/" erstellt
# ulp_embed_binary() erstellt ein Target "ulp_lp_core" (automatisches "ulp_" Präfix)
# und generiert lp_core.h mit den exportierten Variablen und Binary-Symbolen
# Variablen explizit setzen (wie im offiziellen ESP-IDF Beispiel)
set(ulp_app_name "ulp_main")
get_filename_component(ulp_sources "${CMAKE_SOURCE_DIR}/ulp/ulp_main.c" ABSOLUTE)
# set(ulp_sources "../ulp/ulp_main.c")  # LP-Core Source-Datei (wird kompiliert und eingebettet)
set(ulp_exp_dep_srcs "main_idf.cpp")   # main_idf.cpp verwendet lp_core.h (generierter Header)


# Prüfen, ob die benötigte sdkconfig.cmake existiert, um den ULP-Build zu schützen
if(EXISTS "${CMAKE_BINARY_DIR}/config/sdkconfig.cmake")
    ulp_embed_binary(${ulp_app_name} "${ulp_sources}" "${ulp_exp_dep_srcs}")
    
    # Header und Binaries für den HP-Core verfügbar machen
    target_include_directories(${COMPONENT_LIB} PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/${ulp_app_name}.bin.S" PROPERTIES EXTERNAL_OBJECT TRUE)
else()
    message(WARNING "Warte auf Generierung der sdkconfig.cmake... (Build ggf. erneut starten)")
endif()

# 4. FIX für "lp_core.h: No such file or directory"
# Wir fügen das Build-Verzeichnis der Komponente zu den Include-Pfaden hinzu,
# damit main_idf.cpp die generierte Datei findet.
target_include_directories(${COMPONENT_LIB} PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

# 5. FIX für den Assembler-Fehler (Source not found)
# In manchen PlatformIO/CMake-Konstellationen muss das generierte Verzeichnis 
# explizit als Linker-Pfad bekannt gemacht werden.
target_link_directories(${COMPONENT_LIB} PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

# 6. FIX: Den Pfad zum Build-Verzeichnis der Komponente bekannt machen
# Dies hilft PlatformIO/SCons, die Datei 'lp_core.bin.S' zu finden
set_property(TARGET ${COMPONENT_LIB} APPEND PROPERTY 
             INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")

# 7. FIX: Sicherstellen, dass die generierte Assembler-Datei als "Generated" markiert ist
# Das verhindert, dass PlatformIO sie sucht, BEVOR sie erstellt wurde
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/${ulp_app_name}.bin.S" PROPERTIES GENERATED TRUE)

