#ifndef BLE_CONFIG_H
#define BLE_CONFIG_H

#include "hardware.h"

// ============================================
// BLE-Stack: NimBLE (Espressif-Empfehlung ESP32-C6)
// Stack-Optionen (CONFIG_BT_NIMBLE_*) in sdkconfig.defaults
// ============================================

// ============================================
// Standard-Services (Well-Known UUIDs)
// ============================================

// Current Time Service (CTS) – Zeit-Synchronisation
#define BLE_CTS_SERVICE_UUID            0x1805
#define BLE_CTS_CURRENT_TIME_UUID       0x2A2B  // Read + Write (Exact Time 256, UTC)

// Device Information Service
#define BLE_DIS_SERVICE_UUID            0x180A
#define BLE_DIS_FIRMWARE_REV_UUID       0x2A26  // Read (Firmware Revision String)

// ============================================
// Custom Service (Messdaten)
// ============================================

#define BLE_CUSTOM_SERVICE_UUID         0xFFF0
#define BLE_CUSTOM_DATA_CHAR_UUID       0xFFF1  // Read/Notify – JSON: {p, bv, bp, bl}

// ============================================
// Timeouts und Advertising
// ============================================

// Max. Zeit, in der auf eine Verbindung gewartet wird
// Kein Connect in 90 s → Advertising stoppen, Session beenden
#define BLE_ADVERTISING_DURATION_MS     90000

// Gesamt-Timeout der BLE-Session (Sicherheit, Akku)
// Nach Ablauf: BLE deinit, Deep-Sleep
#define BLE_SESSION_TIMEOUT_MS          180000

// Verzögerung nach Connect, bevor das erste Notify (0xFFF1) gesendet wird.
// Gibt dem Central (z. B. Node-RED generic-ble) Zeit für Service Discovery und
// Subscription auf die Characteristic – sonst geht das Notify oft verloren (Missing → Disconnected, keine Daten).
#define BLE_NOTIFY_DELAY_MS             3000

// Advertising-Intervall (beeinflusst Scan-Erkennbarkeit und Stromverbrauch)
// NimBLE erwartet den Wert in 0.625ms Einheiten: 100ms / 0.625 = 160
#define BLE_ADVERTISING_INTERVAL_MS     100
#define BLE_ADV_ITVL                    (BLE_ADVERTISING_INTERVAL_MS * 1000 / 625)

// ============================================
// TX Power
// ============================================

// BLE-Sendeleistung für Advertising (ESP-IDF: esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, …))
// Höchster Wert über API: +20 dBm (ESP_PWR_LVL_P20). ESP32-C6 Chipbereich: -24 .. +21 dBm.
// Default entspricht der ESP-IDF-Standard-Default (bei keinem Runtime-Override).
#define BLE_TX_POWER_DBM                9
#define BLE_TX_POWER_STABILIZE_MS       50

// ============================================
// CTS – Exact Time 256 Format
// ============================================

// Exact Time 256: 10 Bytes
// Year (2) + Month (1) + Day (1) + Hours (1) + Minutes (1) + Seconds (1) + DayOfWeek (1) + Fractions256 (1) + AdjustReason (1)
#define BLE_CTS_EXACT_TIME_256_LEN      10

// Plausibilitätsgrenzen für empfangene Zeit
#define BLE_TIME_MIN_YEAR               2024
#define BLE_TIME_MAX_YEAR               2099

#endif // BLE_CONFIG_H
