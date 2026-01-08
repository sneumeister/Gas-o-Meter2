#include "transfer_zigbee.h"
#include "zigbee_config.h"
#include "hardware.h"
#include "version.h"

#ifndef ARDUINO
    #include "esp_log.h"
    #include "nvs.h"
    #include "nvs_flash.h"
    static const char* TAG = "transfer_zigbee";
#else
    #include "nvs.h"
    #include "nvs_flash.h"
    #define ESP_LOGI(...) Serial.printf(__VA_ARGS__); Serial.println()
    #define ESP_LOGW(...) Serial.printf(__VA_ARGS__); Serial.println()
    #define ESP_LOGE(...) Serial.printf(__VA_ARGS__); Serial.println()
#endif

// ============================================
// Globale Variablen
// ============================================
static bool zigbee_initialized = false;

// RTC-RAM Variable für ZigBee-Config (Definition)
// WICHTIG: Deklaration ist in zigbee_config.h als extern
RTC_DATA_ATTR zigbee_rtc_t zigbee_rtc = {
    .joined = false,
    .network_addr = 0xFFFF,    // Ungültig (0xFFFF = Broadcast/ungültig)
    .coord_addr = 0x0000,
    .pan_id = 0x0000,
    .channel = 0,
    .extended_addr = 0x0000000000000000ULL
};

// ============================================
// NVS-Funktionen für ZigBee-Config
// ============================================

/**
 * @brief Lädt ZigBee-Config aus NVS in RTC-RAM
 * 
 * @param is_power_on true bei Power-On (NVS laden), false bei Deep-Sleep-Wake-up (RTC-RAM verwenden)
 * @return true bei Erfolg, false bei Fehler
 */
bool zigbee_config_load_from_nvs(bool is_power_on) {
    if (!is_power_on) {
        // Deep-Sleep-Wake-up: RTC-RAM ist noch vorhanden → prüfe Gültigkeit
        if (zigbee_rtc.joined || ZIGBEE_IS_NETWORK_ADDR_VALID(zigbee_rtc.network_addr)) {
            // RTC-RAM enthält gültige Daten → verwenden
            ESP_LOGI(TAG, "zigbee_config_load_from_nvs: Deep-Sleep-Wake-up → verwende RTC-RAM (joined: %s, network_addr: 0x%04X)",
                     zigbee_rtc.joined ? "true" : "false", zigbee_rtc.network_addr);
            return true;
        } else {
            // RTC-RAM enthält ungültige Daten (z.B. beim ersten Deep-Sleep-Wake-up nach Power-On) → aus NVS laden
            ESP_LOGI(TAG, "zigbee_config_load_from_nvs: Deep-Sleep-Wake-up, aber RTC-RAM ungültig → lade aus NVS");
            // Weiter mit NVS-Laden (siehe unten)
        }
    }
    
    // Power-On oder ungültiger RTC-RAM: RTC-RAM könnte zufällige Werte enthalten → explizit initialisieren
    // Zuerst RTC-RAM auf Default-Werte setzen (sicherstellen, dass keine Zufallswerte verwendet werden)
    zigbee_rtc_t default_config = {
        .joined = false,
        .network_addr = 0xFFFF,    // Ungültig
        .coord_addr = 0x0000,
        .pan_id = 0x0000,
        .channel = 0,
        .extended_addr = 0x0000000000000000ULL
    };
    zigbee_rtc = default_config;
    
    // Versuche aus NVS zu laden
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open(ZIGBEE_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "zigbee_config_load_from_nvs: NVS-Namespace '%s' existiert nicht (noch nicht gepaart) → RTC-RAM auf Default-Werte gesetzt", 
                 ZIGBEE_NVS_NAMESPACE);
        // RTC-RAM ist bereits auf Default-Werte gesetzt
        return true;  // Kein Fehler, einfach noch nicht gepaart
    }
    
    // Lade alle Werte aus NVS
    size_t required_size = sizeof(zigbee_rtc_t);
    err = nvs_get_blob(nvs_handle, "config", &zigbee_rtc, &required_size);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK && required_size == sizeof(zigbee_rtc_t)) {
        // Validiere geladene Daten
        if (zigbee_rtc.joined && ZIGBEE_IS_NETWORK_ADDR_VALID(zigbee_rtc.network_addr)) {
            ESP_LOGI(TAG, "zigbee_config_load_from_nvs: Config aus NVS geladen (joined: %s, network_addr: 0x%04X, pan_id: 0x%04X, channel: %d)",
                     zigbee_rtc.joined ? "true" : "false", zigbee_rtc.network_addr, zigbee_rtc.pan_id, zigbee_rtc.channel);
            return true;
        } else {
            // Geladene Daten sind ungültig → auf Default-Werte zurücksetzen
            ESP_LOGW(TAG, "zigbee_config_load_from_nvs: Config in NVS gefunden, aber ungültig (joined: %s, network_addr: 0x%04X) → RTC-RAM auf Default-Werte gesetzt",
                     zigbee_rtc.joined ? "true" : "false", zigbee_rtc.network_addr);
            zigbee_rtc = default_config;
            return true;
        }
    } else {
        ESP_LOGI(TAG, "zigbee_config_load_from_nvs: Config nicht in NVS gefunden (noch nicht gepaart) → RTC-RAM auf Default-Werte gesetzt");
        // RTC-RAM ist bereits auf Default-Werte gesetzt
        return true;  // Kein Fehler, einfach noch nicht gepaart
    }
}

/**
 * @brief Speichert ZigBee-Config aus RTC-RAM in NVS
 * 
 * Wird nach erfolgreichem Pairing aufgerufen.
 * 
 * @return true bei Erfolg, false bei Fehler
 */
bool zigbee_config_save_to_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open(ZIGBEE_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        // Namespace existiert nicht → erstellen
        err = nvs_open(ZIGBEE_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "zigbee_config_save_to_nvs: NVS-Namespace '%s' konnte nicht geöffnet werden: %s",
                     ZIGBEE_NVS_NAMESPACE, esp_err_to_name(err));
            return false;
        }
    }
    
    // Speichere gesamte Config-Struktur als Blob
    err = nvs_set_blob(nvs_handle, "config", &zigbee_rtc, sizeof(zigbee_rtc_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "zigbee_config_save_to_nvs: Fehler beim Speichern: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    // Commit
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "zigbee_config_save_to_nvs: Fehler beim Committen: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "zigbee_config_save_to_nvs: Config in NVS gespeichert (joined: %s, network_addr: 0x%04X, pan_id: 0x%04X, channel: %d)",
             zigbee_rtc.joined ? "true" : "false", zigbee_rtc.network_addr, zigbee_rtc.pan_id, zigbee_rtc.channel);
    return true;
}

/**
 * @brief Initialisiert ZigBee-Config (lädt aus NVS bei Power-On)
 * 
 * @param is_power_on true bei Power-On, false bei Deep-Sleep-Wake-up
 * @return true bei Erfolg, false bei Fehler
 */
bool zigbee_config_init(bool is_power_on) {
    return zigbee_config_load_from_nvs(is_power_on);
}

// ============================================
// Dummy-Implementierung (für Test)
// ============================================

bool transfer_zigbee_init(void) {
    if (zigbee_initialized) {
        ESP_LOGW(TAG, "transfer_zigbee_init: Bereits initialisiert");
        return true;
    }
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ZigBee-Stack Initialisierung (DUMMY)");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  [1/5] ZigBee-Stack wird initialisiert...");
    ESP_LOGI(TAG, "  [2/5] Konfiguration wird geladen...");
    ESP_LOGI(TAG, "  [3/5] Hardware wird vorbereitet...");
    ESP_LOGI(TAG, "  [4/5] Stack wird gestartet...");
    ESP_LOGI(TAG, "  [5/5] Initialisierung abgeschlossen");
    ESP_LOGI(TAG, "========================================");
    
    zigbee_initialized = true;
    return true;
}

transfer_status_t transfer_zigbee_send_data(const transfer_data_t* data) {
    if (data == nullptr) {
        ESP_LOGE(TAG, "transfer_zigbee_send_data: data ist NULL");
        return TRANSFER_STATUS_UNKNOWN_ERROR;
    }
    
    if (!zigbee_initialized) {
        ESP_LOGE(TAG, "transfer_zigbee_send_data: ZigBee nicht initialisiert");
        return TRANSFER_STATUS_INIT_FAILED;
    }
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ZigBee-Datenübertragung (DUMMY)");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  [1/4] Verbindungsaufbau zum Coordinator...");
    ESP_LOGI(TAG, "        → Verbindung erfolgreich (DUMMY)");
    ESP_LOGI(TAG, "  [2/4] Sende Daten:");
    ESP_LOGI(TAG, "        → pulse_counter: %lu", data->pulse_counter);
    ESP_LOGI(TAG, "        → battery_percent: %.1f%%", data->battery_percent);
    ESP_LOGI(TAG, "        → firmware_version: %s", data->firmware_version ? data->firmware_version : "N/A");
    ESP_LOGI(TAG, "        → Daten erfolgreich gesendet (DUMMY)");
    ESP_LOGI(TAG, "  [3/4] Zeit-Synchronisation...");
    
    // Zeit-Synchronisation (Dummy)
    if (transfer_zigbee_sync_time()) {
        ESP_LOGI(TAG, "        → Zeit erfolgreich synchronisiert (DUMMY)");
    } else {
        ESP_LOGW(TAG, "        → Zeit-Synchronisation fehlgeschlagen (DUMMY)");
    }
    
    ESP_LOGI(TAG, "  [4/4] Abmelden vom Coordinator...");
    ESP_LOGI(TAG, "        → Abmeldung erfolgreich (DUMMY)");
    ESP_LOGI(TAG, "========================================");
    
    return TRANSFER_STATUS_OK;
}

bool transfer_zigbee_sync_time(void) {
    if (!zigbee_initialized) {
        ESP_LOGE(TAG, "transfer_zigbee_sync_time: ZigBee nicht initialisiert");
        return false;
    }
    
    ESP_LOGI(TAG, "transfer_zigbee_sync_time: Hole Zeit vom Coordinator (DUMMY)...");
    ESP_LOGI(TAG, "  → Coordinator-Zeit: 2026-01-15 14:30:00 (DUMMY)");
    ESP_LOGI(TAG, "  → ESP-Zeit wird aktualisiert (DUMMY)");
    
    // TODO: Echte Zeit-Synchronisation implementieren
    // - Zeit vom Coordinator abfragen
    // - ESP-Systemzeit aktualisieren (ähnlich NTP)
    
    return true;
}

void transfer_zigbee_deinit(void) {
    if (!zigbee_initialized) {
        return;
    }
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ZigBee-Stack Deinitialisierung (DUMMY)");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  [1/2] Stack wird gestoppt...");
    ESP_LOGI(TAG, "  [2/2] Ressourcen werden freigegeben...");
    ESP_LOGI(TAG, "========================================");
    
    zigbee_initialized = false;
}
