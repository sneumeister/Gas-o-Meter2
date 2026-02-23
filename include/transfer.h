#ifndef TRANSFER_H
#define TRANSFER_H

#include <stdint.h>
#include <stdbool.h>

// ============================================
// Transfer-Mode Definitionen
// ============================================
#define TRANSFER_MODE_NONE    "none"
#define TRANSFER_MODE_ZIGBEE  "zigbee"
#define TRANSFER_MODE_BLE     "ble"
#define TRANSFER_MODE_MQTT    "mqtt"

// ============================================
// Transfer-Status Codes
// ============================================
typedef enum {
    TRANSFER_STATUS_OK = 0,                    // Erfolgreich
    TRANSFER_STATUS_NOT_CONFIGURED = -1,       // Transfer-Mode nicht konfiguriert
    TRANSFER_STATUS_INIT_FAILED = -2,          // Initialisierung fehlgeschlagen
    TRANSFER_STATUS_CONNECTION_FAILED = -3,    // Verbindung fehlgeschlagen
    TRANSFER_STATUS_SEND_FAILED = -4,          // Datenübertragung fehlgeschlagen
    TRANSFER_STATUS_TIME_SYNC_FAILED = -5,     // Zeit-Synchronisation fehlgeschlagen
    TRANSFER_STATUS_CLEANUP_FAILED = -6,       // Aufräumen fehlgeschlagen
    TRANSFER_STATUS_UNKNOWN_ERROR = -99        // Unbekannter Fehler
} transfer_status_t;

// ============================================
// Transfer-Datenstruktur
// ============================================
typedef struct {
    uint32_t pulse_counter;       // Gas-Zählerstand (aus ulp_pulse_counter)
    float battery_percent;         // Akku-Ladezustand in Prozent (0.0 - 100.0)
    float battery_voltage;         // Akku-Spannung in Volt (z.B. 3.57V) - für Zigbee Battery Voltage Attribut
    const char* firmware_version;  // Firmware-Version (z.B. "0.6")
} transfer_data_t;

// ============================================
// Transfer-Funktionsprototypen
// ============================================

/**
 * @brief Initialisiert den Transfer-Modus (einmalig beim Start)
 * 
 * @param transfer_mode Transfer-Modus ("zigbee", "ble", "mqtt", "none")
 * @return true bei Erfolg, false bei Fehler
 */
bool transfer_init(const char* transfer_mode);

/**
 * @brief Führt die Datenübertragung durch
 * 
 * @param data Zu übertragende Daten (pulse_counter, battery_percent, firmware_version)
 * @return transfer_status_t Status-Code (TRANSFER_STATUS_OK bei Erfolg)
 */
transfer_status_t transfer_data(const transfer_data_t* data);

/**
 * @brief Deinitialisiert den Transfer-Modus (beim Shutdown)
 */
void transfer_deinit(void);

/**
 * @brief Gibt eine lesbare Status-Meldung zurück
 * 
 * @param status Status-Code
 * @return const char* Lesbare Fehlermeldung
 */
const char* transfer_status_to_string(transfer_status_t status);

#endif // TRANSFER_H
