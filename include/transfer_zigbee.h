
#ifndef TRANSFER_ZIGBEE_H
#define TRANSFER_ZIGBEE_H

#include "transfer.h"

#ifndef ARDUINO
    #include "esp_err.h"  // Für esp_err_t
#endif

// ============================================
// ZigBee-spezifische Funktionen
// ============================================

/**
 * @brief Initialisiert den ZigBee-Stack
 * 
 * @return true bei Erfolg, false bei Fehler
 */
bool transfer_zigbee_init(void);

/**
 * @brief Führt die ZigBee-Datenübertragung durch
 * 
 * @param data Zu übertragende Daten
 * @return transfer_status_t Status-Code
 */
transfer_status_t transfer_zigbee_send_data(const transfer_data_t* data);

/**
 * @brief Synchronisiert die Zeit vom ZigBee-Coordinator
 * 
 * @return true bei Erfolg, false bei Fehler
 */
bool transfer_zigbee_sync_time(void);

/**
 * @brief Deinitialisiert den ZigBee-Stack
 */
void transfer_zigbee_deinit(void);

/**
 * @brief Gibt den aktuellen ZigBee-Status als JSON-String zurück
 * 
 * @param buffer Buffer für JSON-String (mindestens 512 Bytes)
 * @param buffer_size Größe des Buffers
 * @return true bei Erfolg, false bei Fehler
 */
bool transfer_zigbee_get_status_json(char* buffer, size_t buffer_size);

/**
 * @brief Führt einen Factory-Reset durch (löscht alle ZigBee-Daten)
 * 
 * @param transfer_mode Aktueller Transfer-Modus (um zu prüfen, ob ZigBee aktiv ist)
 * @return true bei Erfolg, false bei Fehler
 */
bool transfer_zigbee_factory_reset(const char* transfer_mode);

/**
 * @brief Startet ZigBee-Pairing (Network Steering)
 * 
 * @return esp_err_t ESP_OK bei Erfolg, Fehlercode bei Fehler
 */
esp_err_t transfer_zigbee_start_pairing(void);

/**
 * @brief Prüft, ob ein Factory-Reset gerade läuft
 * 
 * @return true wenn Factory-Reset läuft, false sonst
 */
bool transfer_zigbee_is_factory_reset_in_progress(void);

/**
 * @brief Stellt sicher, dass das Device mit dem ZigBee-Netzwerk verbunden ist
 * 
 * Prüft den aktuellen Status (factory-new, joined) und startet bei Bedarf
 * Pairing oder Rejoin. Wartet auf erfolgreichen Abschluss mit Timeouts.
 * 
 * @return transfer_status_t TRANSFER_STATUS_OK wenn verbunden, Fehlercode bei Fehler
 */
transfer_status_t transfer_zigbee_ensure_joined(void);

/**
 * @brief Prüft, ob der ZigBee-Stack initialisiert ist
 * 
 * @return true wenn Stack initialisiert ist, false sonst
 */
bool transfer_zigbee_is_initialized(void);

/**
 * @brief Prüft, ob das Device mit dem ZigBee-Netzwerk verbunden ist
 * 
 * @return true wenn Device joined ist, false sonst
 */
bool transfer_zigbee_is_joined(void);

/**
 * @brief Prüft, ob das Device factory-new ist (noch nicht gepaart)
 * 
 * @return true wenn Device factory-new ist, false sonst
 */
bool transfer_zigbee_is_factory_new(void);

#endif // TRANSFER_ZIGBEE_H
