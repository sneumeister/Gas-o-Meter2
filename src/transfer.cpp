#include "transfer.h"
#include "transfer_zigbee.h"
#include "hardware.h"
#include <string.h>  // Für strcmp

#ifndef ARDUINO
    #include "esp_log.h"
    static const char* TAG = "transfer";
#else
    #define ESP_LOGI(...) Serial.printf(__VA_ARGS__); Serial.println()
    #define ESP_LOGW(...) Serial.printf(__VA_ARGS__); Serial.println()
    #define ESP_LOGE(...) Serial.printf(__VA_ARGS__); Serial.println()
#endif

// ============================================
// Globale Variablen
// ============================================
static const char* current_transfer_mode = TRANSFER_MODE_NONE;
static bool transfer_initialized = false;

// ============================================
// Basis-Implementierung
// ============================================

bool transfer_init(const char* transfer_mode) {
    if (transfer_mode == nullptr) {
        ESP_LOGE(TAG, "transfer_init: transfer_mode ist NULL");
        return false;
    }
    
    if (transfer_initialized) {
        ESP_LOGW(TAG, "transfer_init: Bereits initialisiert (Mode: %s)", current_transfer_mode);
        return true;
    }
    
    current_transfer_mode = transfer_mode;
    transfer_initialized = true;
    
    ESP_LOGI(TAG, "transfer_init: Transfer-Modus initialisiert: %s", transfer_mode);
    
    // Mode-spezifische Initialisierung wird in transfer_data() gemacht
    return true;
}

transfer_status_t transfer_data(const transfer_data_t* data) {
    if (data == nullptr) {
        ESP_LOGE(TAG, "transfer_data: data ist NULL");
        return TRANSFER_STATUS_UNKNOWN_ERROR;
    }
    
    if (!transfer_initialized) {
        ESP_LOGE(TAG, "transfer_data: Transfer nicht initialisiert");
        return TRANSFER_STATUS_NOT_CONFIGURED;
    }
    
    // Mode-spezifische Implementierung
    if (strcmp(current_transfer_mode, TRANSFER_MODE_NONE) == 0) {
        ESP_LOGI(TAG, "transfer_data: Transfer-Modus 'none' → keine Übertragung");
        return TRANSFER_STATUS_OK;
    }
    
    // ZigBee-Implementierung
    if (strcmp(current_transfer_mode, TRANSFER_MODE_ZIGBEE) == 0) {
        // ZigBee-Stack initialisieren (falls noch nicht geschehen)
        if (!transfer_zigbee_init()) {
            ESP_LOGE(TAG, "transfer_data: ZigBee-Initialisierung fehlgeschlagen");
            return TRANSFER_STATUS_INIT_FAILED;
        }
        
        // Daten senden
        return transfer_zigbee_send_data(data);
    }
    
    // Für andere Modi wird die spezifische Implementierung aufgerufen
    // (wird in transfer_ble.cpp, transfer_mqtt.cpp, etc. implementiert)
    ESP_LOGW(TAG, "transfer_data: Transfer-Modus '%s' noch nicht implementiert", current_transfer_mode);
    return TRANSFER_STATUS_NOT_CONFIGURED;
}

void transfer_deinit(void) {
    if (!transfer_initialized) {
        return;
    }
    
    // Mode-spezifische Deinitialisierung
    if (strcmp(current_transfer_mode, TRANSFER_MODE_ZIGBEE) == 0) {
        transfer_zigbee_deinit();
    }
    // Weitere Modi hier hinzufügen (BLE, MQTT, etc.)
    
    ESP_LOGI(TAG, "transfer_deinit: Transfer-Modus deinitialisiert (Mode: %s)", current_transfer_mode);
    current_transfer_mode = TRANSFER_MODE_NONE;
    transfer_initialized = false;
}

const char* transfer_status_to_string(transfer_status_t status) {
    switch (status) {
        case TRANSFER_STATUS_OK:
            return "OK";
        case TRANSFER_STATUS_NOT_CONFIGURED:
            return "Nicht konfiguriert";
        case TRANSFER_STATUS_INIT_FAILED:
            return "Initialisierung fehlgeschlagen";
        case TRANSFER_STATUS_CONNECTION_FAILED:
            return "Verbindung fehlgeschlagen";
        case TRANSFER_STATUS_SEND_FAILED:
            return "Datenübertragung fehlgeschlagen";
        case TRANSFER_STATUS_TIME_SYNC_FAILED:
            return "Zeit-Synchronisation fehlgeschlagen";
        case TRANSFER_STATUS_CLEANUP_FAILED:
            return "Aufräumen fehlgeschlagen";
        case TRANSFER_STATUS_UNKNOWN_ERROR:
        default:
            return "Unbekannter Fehler";
    }
}
