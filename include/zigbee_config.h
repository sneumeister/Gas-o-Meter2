#ifndef ZIGBEE_CONFIG_H
#define ZIGBEE_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware.h"  // Für PULSE_COUNTER_DIVISOR

// ============================================
// ZigBee Device Configuration (Compile-Time)
// ============================================

// Device-Identifikation (für Zigbee2MQTT Erkennung)
#define ZIGBEE_MODEL_ID           "Gas-O-Meter2"
#define ZIGBEE_MANUFACTURER_NAME  "Custom"
#define ZIGBEE_DEVICE_TYPE        ESP_ZB_DEVICE_TYPE_END_DEVICE  // Wird in ESP-Zigbee-SDK definiert

// Profile & Device ID Konfiguration
// WICHTIG: Diese Werte müssen mit den ESP-Zigbee-SDK Headers übereinstimmen
// Include: zcl/esp_zigbee_zcl_common.h für ESP_ZB_AF_HA_PROFILE_ID, ESP_ZB_HA_CUSTOM_ATTR_DEVICE_ID
#define ZIGBEE_PROFILE_ID         ESP_ZB_AF_HA_PROFILE_ID  // Home Automation Profile (0x0104)
#define ZIGBEE_DEVICE_ID          ESP_ZB_HA_CUSTOM_ATTR_DEVICE_ID  // Custom Device (0xFFFF)
#define ZIGBEE_DEVICE_VERSION     0  // Device Version

// Cluster-IDs (Standard ZCL - ZigBee Cluster Library)
#define ZIGBEE_CLUSTER_BATTERY        0x0001  // Power Configuration Cluster
#define ZIGBEE_CLUSTER_METERING       0x0702  // Simple Metering Cluster (für Gas-Zähler)
#define ZIGBEE_CLUSTER_BASIC          0x0000  // Basic Cluster (für Firmware-Version)

// Attribute-IDs
#define ZIGBEE_ATTR_BATTERY_PERCENT   0x0021  // Battery Percentage Remaining (0-200, 0-100%)
#define ZIGBEE_ATTR_BATTERY_VOLTAGE   0x0020  // Battery Voltage (uint8 in 100mV Einheiten, z.B. 35 = 3.5V)
#define ZIGBEE_ATTR_BATTERY_ALARM_STATE 0x003E  // Battery Alarm State (map32 Bitmap, Bit 0 = Low Voltage Alarm)
#define ZIGBEE_ATTR_METERING_CURRENT_SUMMATION_DELIVERED  0x0000  // CurrentSummationDelivered (Metering Cluster)
#define ZIGBEE_ATTR_APP_VERSION       0x0001  // Application Version (Basic Cluster)

// Metering Cluster Konfiguration
// Unit of Measure: m³ (m3, m3/h binary value)
// WICHTIG: Diese Werte müssen mit esp_zigbee_zcl_metering.h übereinstimmen
#define ZIGBEE_METERING_UNIT_OF_MEASURE   ESP_ZB_ZCL_METERING_UNIT_M3_M3H_BINARY  // 0x01: m³ binary
#define ZIGBEE_METERING_DEVICE_TYPE       ESP_ZB_ZCL_METERING_GAS_METERING  // 1: Gas metering device
#define ZIGBEE_METERING_MULTIPLIER        1  // Multiplier für Skalierung (1 = keine Multiplikation)
// Divisor aus hardware.h verwenden (zentrale Definition für alle Verwendungen)
#ifndef PULSE_COUNTER_DIVISOR
    #error "PULSE_COUNTER_DIVISOR muss in hardware.h definiert sein!"
#endif
#define ZIGBEE_METERING_DIVISOR           PULSE_COUNTER_DIVISOR  // Divisor: Integer-Wert wird durch PULSE_COUNTER_DIVISOR geteilt
// Beispiel: pulse_counter = 12345 → CurrentSummationDelivered = 123.45 m³ (bei PULSE_COUNTER_DIVISOR = 100)

// Reporting-Intervalle (in Sekunden)
// HINWEIS: Diese Werte sind Defaults/Vorschläge für Zigbee2MQTT configureReporting()
// Die tatsächliche Konfiguration erfolgt durch den Coordinator beim Pairing oder später.
// Reporting kann jederzeit vom Coordinator neu konfiguriert werden (nicht nur beim ersten Pairing).
#define ZIGBEE_BATTERY_REPORT_MIN     3600    // Minimum: 1 Stunde
#define ZIGBEE_BATTERY_REPORT_MAX     86400   // Maximum: 24 Stunden
#define ZIGBEE_BATTERY_REPORT_CHANGE  1       // Report bei 1% Änderung
// Metering Reporting: ZEITBASIERT (Wake-Up Intervall × Multiplier)
// Da wir rein zeitbasiert übertragen, ist ReportableChange = 0 (jede Änderung wird gemeldet)
#define ZIGBEE_METERING_REPORT_MIN    300     // Minimum: 5 Minuten (kann im Web-UI angepasst werden)
#define ZIGBEE_METERING_REPORT_MAX    3600    // Maximum: 1 Stunde (kann im Web-UI angepasst werden)
#define ZIGBEE_METERING_REPORT_CHANGE 0       // Report bei JEDER Änderung (zeitbasierte Übertragung, keine Mindeständerung)

// Retry & Timeout Konfiguration
#define ZIGBEE_JOIN_RETRY_COUNT       3       // Anzahl Join-Versuche
#define ZIGBEE_JOIN_TIMEOUT_MS        30000   // 30 Sekunden pro Join-Versuch
#define ZIGBEE_DATA_RETRY_COUNT       3       // Anzahl Daten-Übertragungs-Versuche
#define ZIGBEE_DATA_TIMEOUT_MS        5000    // 5 Sekunden pro Paket
#define ZIGBEE_NETWORK_DISCOVERY_MS   30000   // 30 Sekunden für Network Discovery
#define ZIGBEE_PAIRING_TIMEOUT_MS     180000  // 3 Minuten (180 Sekunden) für Pairing-Timeout (Standard nach ZigBee-Spezifikation und Best Practice)
#define ZIGBEE_CYCLE_TIMEOUT_MS       300000  // 5 Minuten (300 Sekunden) Gesamt-Timeout für gesamten ZigBee-Zyklus (Pairing + Rejoin + Datenübertragung)
#define ZIGBEE_INIT_TIMEOUT_MS        5000    // 5 Sekunden Timeout für Stack-Initialisierung
#define ZIGBEE_INIT_POLL_INTERVAL_MS  100     // Poll-Intervall für Stack-Initialisierung
#define ZIGBEE_STEERING_POLL_INTERVAL_MS 500  // Poll-Intervall für Network Steering (500ms = weniger CPU-Last, aber immer noch responsiv)
#define ZIGBEE_STEERING_RETRY_COUNT   3       // Anzahl Retry-Versuche bei Network Steering FAIL
#define ZIGBEE_STEERING_RETRY_TIMER_MS 3000   // Wartezeit zwischen Retry-Versuchen (3 Sekunden = gibt Coordinator Zeit, Permit Join zu aktivieren)
#define ZIGBEE_INTERVIEW_WAIT_MS      90000   // 90 Sekunden Wartezeit nach erstem Pairing für Interview (Zigbee2MQTT benötigt Zeit für Active Endpoints, Simple Descriptor, etc.)

// Timing-Konfiguration für Association Request nach Network Steering
// WICHTIG: Nach erfolgreichem Network Steering benötigt der Stack Zeit, um:
// - Network-Informationen zu verarbeiten
// - MAC-Layer zu stabilisieren
// - Association Request vorzubereiten
// Bekanntes Problem (SDK Issue #335/TZ-842): Coordinator sendet manchmal keine Association Response
// wenn die Request zu früh nach Network Discovery kommt
// Empfohlene Werte: 500-2000 ms (abhängig von Netzwerk-Latenz)
#define ZIGBEE_STEERING_TO_ASSOCIATION_DELAY_MS  1500  // Verzögerung nach erfolgreichem Network Steering vor Association Request (ms)

// TX Power Konfiguration (Sendeleistung)
// ESP32-C6 Maximum: +10 dBm, Minimum: SDK-abhängig (typisch -30 dBm)
// Höhere Werte = größere Reichweite, aber höherer Stromverbrauch
#define ZIGBEE_TX_POWER_DEFAULT       10       // Default TX Power: +10 dBm (Maximum für beste Reichweite)

// Minimum LQI für Network Join (Link Quality Indicator)
// LQI-Bereich: 0-255 (0 = schlechteste Qualität, 255 = beste Qualität)
// Default im SDK: 32 (konservativ, verhindert Join bei schwachem Signal)
// WICHTIG: Bei LQI=0 oder sehr niedrigen Werten kann Join fehlschlagen
// Empfehlung: 0 = erlaubt Join auch bei sehr schwachem Signal (für Debugging/Test)
//             32 = Standard (empfohlen für Produktion, verhindert instabile Verbindungen)
#define ZIGBEE_MIN_JOIN_LQI           0        // Minimum LQI für Network Join (0 = deaktiviert, erlaubt Join auch bei LQI=0)

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
// End Device Timeout (Aging Timeout): Zeit, nach der der Parent das Device als inaktiv betrachtet
// WICHTIG: keep_alive muss kleiner als ed_timeout sein, sonst wird das Device vom Parent entfernt
// Empfohlene Werte: ESP_ZB_ED_AGING_TIMEOUT_64MIN (64 Minuten) für batteriebetriebene Devices
// Verfügbare Werte: ESP_ZB_ED_AGING_TIMEOUT_10SEC, ESP_ZB_ED_AGING_TIMEOUT_2MIN, ESP_ZB_ED_AGING_TIMEOUT_4MIN,
//                   ESP_ZB_ED_AGING_TIMEOUT_8MIN, ESP_ZB_ED_AGING_TIMEOUT_16MIN, ESP_ZB_ED_AGING_TIMEOUT_32MIN,
//                   ESP_ZB_ED_AGING_TIMEOUT_64MIN (Default), ESP_ZB_ED_AGING_TIMEOUT_128MIN, ESP_ZB_ED_AGING_TIMEOUT_256MIN
// HINWEIS: Die tatsächliche Konstante wird in transfer_zigbee.cpp verwendet (nach Include der SDK-Header)
//          Die Konfiguration erfolgt dort zur Compile-Zeit mit #ifdef-Prüfung

// Keep Alive (Poll Intervall): Wie oft das Device den Parent pollt, um "alive" zu signalisieren
// WICHTIG: Muss kleiner als ed_timeout sein! Empfohlen: 3-10 Sekunden
// Default im SDK: 3000 ms (3 Sekunden)
// Höhere Werte = weniger Stromverbrauch, aber längere Latenz bei Nachrichten
// Niedrigere Werte = schnellere Nachrichten, aber höherer Stromverbrauch
#define ZIGBEE_KEEP_ALIVE_DEFAULT     3000           // Default Keep Alive: 3 Sekunden (3000 ms, empfohlen für batteriebetriebene Devices)

// Stabilisierungszeiten (für kritische RF-Operationen)
// WICHTIG: RF-Operationen benötigen Zeit zur Stabilisierung, besonders nach:
// - TX Power Änderungen
// - Node Descriptor Setzung
// - Stack-Start
// - Antennenumschaltung (bereits in hardware.h: 100ms)
#define ZIGBEE_TX_POWER_STABILIZE_MS      50         // Stabilisierungszeit nach TX Power Setzung (ms)
#define ZIGBEE_NODE_DESC_STABILIZE_MS     50         // Stabilisierungszeit nach Node Descriptor Setzung (ms)
#define ZIGBEE_STACK_START_STABILIZE_MS   200        // Stabilisierungszeit nach Stack-Start vor Pairing (ms)

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
