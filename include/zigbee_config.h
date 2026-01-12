#ifndef ZIGBEE_CONFIG_H
#define ZIGBEE_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

// ============================================
// ZigBee Device Configuration (Compile-Time)
// ============================================

// Device-Identifikation (für Zigbee2MQTT Erkennung)
#define ZIGBEE_MODEL_ID           "Gas-O-Meter2"
#define ZIGBEE_MANUFACTURER_NAME  "Custom"
#define ZIGBEE_DEVICE_TYPE        ESP_ZB_DEVICE_TYPE_END_DEVICE  // Wird in ESP-Zigbee-SDK definiert

// Cluster-IDs (Standard ZCL - ZigBee Cluster Library)
#define ZIGBEE_CLUSTER_BATTERY        0x0001  // Power Configuration Cluster
#define ZIGBEE_CLUSTER_ANALOG_INPUT   0x0400  // Analog Input Cluster (für Gas-Zähler)
#define ZIGBEE_CLUSTER_BASIC          0x0000  // Basic Cluster (für Firmware-Version)

// Attribute-IDs
#define ZIGBEE_ATTR_BATTERY_PERCENT   0x0021  // Battery Percentage Remaining (0-200, 0-100%)
#define ZIGBEE_ATTR_ANALOG_VALUE      0x0055  // Present Value (Analog Input)
#define ZIGBEE_ATTR_APP_VERSION       0x0001  // Application Version (Basic Cluster)

// Reporting-Intervalle (in Sekunden)
#define ZIGBEE_BATTERY_REPORT_MIN     3600    // Minimum: 1 Stunde
#define ZIGBEE_BATTERY_REPORT_MAX     86400   // Maximum: 24 Stunden
#define ZIGBEE_BATTERY_REPORT_CHANGE  1       // Report bei 1% Änderung
#define ZIGBEE_ANALOG_REPORT_MIN      300     // Minimum: 5 Minuten
#define ZIGBEE_ANALOG_REPORT_MAX      3600    // Maximum: 1 Stunde
#define ZIGBEE_ANALOG_REPORT_CHANGE   1       // Report bei 1 Pulse Änderung

// Retry & Timeout Konfiguration
#define ZIGBEE_JOIN_RETRY_COUNT       3       // Anzahl Join-Versuche
#define ZIGBEE_JOIN_TIMEOUT_MS        30000   // 30 Sekunden pro Join-Versuch
#define ZIGBEE_DATA_RETRY_COUNT       3       // Anzahl Daten-Übertragungs-Versuche
#define ZIGBEE_DATA_TIMEOUT_MS        5000    // 5 Sekunden pro Paket
#define ZIGBEE_NETWORK_DISCOVERY_MS   30000   // 30 Sekunden für Network Discovery
#define ZIGBEE_PAIRING_TIMEOUT_MS     300000  // 5 Minuten (300 Sekunden) für Pairing-Timeout (Network Steering kann bis zu 3-5 Minuten dauern)
#define ZIGBEE_CYCLE_TIMEOUT_MS       420000  // 7 Minuten (420 Sekunden) Gesamt-Timeout für gesamten ZigBee-Zyklus (Pairing + Rejoin + Datenübertragung)

// Network Channel Konfiguration
// Channel Mask: 0x07FFF800 = alle Channels 11-26 (Standard ZigBee 2.4 GHz)
// Bits: Channel 11 = Bit 11, Channel 12 = Bit 12, ..., Channel 26 = Bit 26
#define ZIGBEE_PRIMARY_CHANNEL_MASK   0x07FFF800UL  // Alle Channels 11-26

// Endpoint Konfiguration
#define ZIGBEE_ENDPOINT_ID            1              // ZigBee Endpoint ID

// Default-Werte für zigbee_rtc_t (ungültige/Initial-Werte)
#define ZIGBEE_INVALID_NETWORK_ADDR   0xFFFF         // Ungültige Network Address (Broadcast)
#define ZIGBEE_DEFAULT_COORD_ADDR     0x0000         // Default Coordinator Address
#define ZIGBEE_DEFAULT_PAN_ID         0x0000         // Default PAN ID
#define ZIGBEE_DEFAULT_CHANNEL        0              // Default Channel (ungültig)
#define ZIGBEE_DEFAULT_EXTENDED_ADDR  0x0000000000000000ULL  // Default Extended Address

// ZigBee Stack Konfiguration (End Device)
#define ZIGBEE_ED_TIMEOUT_DEFAULT     0              // Default End Device Timeout
#define ZIGBEE_KEEP_ALIVE_DEFAULT     0              // Default Keep Alive (ms)
#define ZIGBEE_INSTALL_CODE_POLICY_DEFAULT  false   // Default Install Code Policy

// FreeRTOS Task Konfiguration
#define ZIGBEE_MAIN_TASK_STACK_SIZE   4096           // Stack Size für ZigBee Main Loop Task
#define ZIGBEE_MAIN_TASK_PRIORITY     5              // Task Priority (0-25, höher = wichtiger)
#define ZIGBEE_MAIN_TASK_DELAY_MS     100            // Delay zwischen Main Loop Iterationen (ms)
#define ZIGBEE_DEINIT_DELAY_MS        500            // Delay beim Deinitialisieren (ms)

// NVS Konfiguration
#define ZIGBEE_NVS_NAMESPACE          "zigbee_config"
#define ZIGBEE_NVS_KEY_CONFIG          "config"      // Key für gesamte Config-Struktur (Blob)
#define ZIGBEE_NVS_KEY_NETWORK_ADDR   "network_addr"
#define ZIGBEE_NVS_KEY_EXTENDED_ADDR  "extended_addr"
#define ZIGBEE_NVS_KEY_PAN_ID         "pan_id"
#define ZIGBEE_NVS_KEY_CHANNEL        "channel"
#define ZIGBEE_NVS_KEY_COORD_ADDR     "coord_addr"
#define ZIGBEE_NVS_KEY_JOINED         "joined"

// ============================================
// ZigBee Runtime Configuration (RTC-RAM)
// ============================================
// Wird bei Power-On aus NVS geladen, bei Deep-Sleep-Wake-up direkt aus RTC-RAM verwendet

#ifndef ARDUINO
    // ESP-IDF: RTC_DATA_ATTR wird von esp_attr.h definiert
    #include "esp_attr.h"
#else
    // Arduino: RTC_DATA_ATTR wird von ESP32 Core definiert
    #include "esp_sleep.h"
#endif

typedef struct {
    bool joined;                // Join-Status (true = gepaart, false = nicht gepaart)
    uint16_t network_addr;     // Network Address (wird beim Join zugewiesen, 0xFFFF = ungültig)
    uint16_t coord_addr;       // Coordinator Network Address
    uint16_t pan_id;           // PAN ID (Personal Area Network ID)
    uint8_t channel;           // ZigBee Channel (11-26)
    uint64_t extended_addr;    // Extended Address (64-bit MAC-Adresse)
} zigbee_rtc_t;

// RTC-RAM Variable (behält Daten bei Deep-Sleep, verliert bei Power-On)
// WICHTIG: Definition erfolgt in transfer_zigbee.cpp mit RTC_DATA_ATTR, hier nur extern-Deklaration
// RTC_DATA_ATTR wird NICHT bei extern-Deklarationen verwendet (nur bei Definitionen)
extern zigbee_rtc_t zigbee_rtc;

// ============================================
// Hilfs-Makros
// ============================================

// Prüft, ob Network Address gültig ist
#define ZIGBEE_IS_NETWORK_ADDR_VALID(addr)  ((addr) != 0xFFFF)

// Prüft, ob Device gepaart ist
#define ZIGBEE_IS_JOINED()  (zigbee_rtc.joined)

// ============================================
// Funktionsprototypen (implementiert in transfer_zigbee.cpp)
// ============================================

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialisiert ZigBee-Config (lädt aus NVS bei Power-On)
 * 
 * @param is_power_on true bei Power-On (NVS laden), false bei Deep-Sleep-Wake-up (RTC-RAM verwenden)
 * @return true bei Erfolg, false bei Fehler
 */
bool zigbee_config_init(bool is_power_on);

/**
 * @brief Lädt ZigBee-Config aus NVS in RTC-RAM
 * 
 * @param is_power_on true bei Power-On (NVS laden), false bei Deep-Sleep-Wake-up (RTC-RAM verwenden)
 * @return true bei Erfolg, false bei Fehler
 */
bool zigbee_config_load_from_nvs(bool is_power_on);

/**
 * @brief Speichert ZigBee-Config aus RTC-RAM in NVS
 * 
 * Wird nach erfolgreichem Pairing aufgerufen.
 * 
 * @return true bei Erfolg, false bei Fehler
 */
bool zigbee_config_save_to_nvs(void);

#ifdef __cplusplus
}
#endif

#endif // ZIGBEE_CONFIG_H
