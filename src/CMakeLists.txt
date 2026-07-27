# ESP-IDF Component für gas-o-meter2

# PROJECT_DIR ist die ESP-IDF Variable für das Projekt-Root-Verzeichnis
get_filename_component(INCLUDE_DIR "${PROJECT_DIR}/include" ABSOLUTE)

# Optional: TPL5110-Hardwaretest (PlatformIO env TPL_test → -DGOM_TPL_TEST=1)
if(GOM_TPL_TEST)
    message(STATUS "GOM_TPL_TEST=1 → baue nur main_TPL_test.cpp (ohne ULP/WiFi/LittleFS)")
    idf_component_register(
        SRCS "main_TPL_test.cpp"
        INCLUDE_DIRS "${INCLUDE_DIR}"
        REQUIRES
            driver
            esp_timer
            freertos
            log
    )
    return()
endif()

# ArduinoJson Bibliothek einbinden (über PlatformIO lib_deps)
# PlatformIO bindet Bibliotheken automatisch ein, wenn lib_compat_mode = off gesetzt ist

idf_component_register(
    SRCS "main_idf.cpp"
          "transfer.cpp"
          "transfer_zigbee.cpp"
          "transfer_ble.cpp"
          "transfer_mqtt.cpp"
          "time_sync.cpp"
          "wifi_manager.cpp"
    INCLUDE_DIRS "${INCLUDE_DIR}"
    REQUIRES 
        littlefs
        mdns                    # Externe Komponente via Component Manager (idf_component.yml)
        esp_http_server
        esp_adc
        nvs_flash
        esp_wifi
        esp_coex                  # esp_coex_wifi_i154_enable() (WiFi + 802.15.4 auf C6)
        esp_netif
        esp_event
        esp_timer
        driver
        freertos
        ulp                     # ULP-Header für ulp_lp_core.h APIs
        bt                      # Bluetooth (NimBLE) für BLE
        mqtt                    # ESP-IDF MQTT Client (IDF built-in)
        espressif__esp-zboss-lib    # ESP-Zigbee-SDK: ZBOSS Stack (über idf_component.yml)
        espressif__esp-zigbee-lib   # ESP-Zigbee-SDK: Espressif APIs (über idf_component.yml)
        nordesems__esp-captive-portal  # AP Captive Portal (DNS, DHCP 114, OS-Probes)
)
# LittleFS Image Erstellung für ESP-IDF
# WICHTIG: Das erste Argument ist das LABEL aus der partitions.csv
littlefs_create_partition_image(storage "${PROJECT_DIR}/data" FLASH_IN_PROJECT)
message(STATUS "LittleFS: Erstelle Image 'littelfs' aus Verzeichnis: ${PROJECT_DIR}/data")

# LP-Core Binary einbetten
set(ulp_app_name "ulp_main")
get_filename_component(ulp_sources "${CMAKE_SOURCE_DIR}/ulp/ulp_main.c" ABSOLUTE)
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

# 4. FIX für "ulp_main.h: No such file or directory"
# Wir fügen das Build-Verzeichnis der Komponente zu den Include-Pfaden hinzu,
# damit main_idf.cpp die generierte Datei findet.
target_include_directories(${COMPONENT_LIB} PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

# 5. FIX für den Assembler-Fehler (Source not found)
# In manchen PlatformIO/CMake-Konstellationen muss das generierte Verzeichnis 
# explizit als Linker-Pfad bekannt gemacht werden.
target_link_directories(${COMPONENT_LIB} PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

# 6. FIX: Den Pfad zum Build-Verzeichnis der Komponente bekannt machen
# Dies hilft PlatformIO/SCons, die Datei 'ulp_main.bin.S' zu finden
set_property(TARGET ${COMPONENT_LIB} APPEND PROPERTY 
             INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")

# 7. FIX: Sicherstellen, dass die generierte Assembler-Datei als "Generated" markiert ist
# Das verhindert, dass PlatformIO sie sucht, BEVOR sie erstellt wurde
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/${ulp_app_name}.bin.S" PROPERTIES GENERATED TRUE)
