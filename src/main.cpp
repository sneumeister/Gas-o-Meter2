#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#define TEMPLATE_PLACEHOLDER '`'   // Als Compiler-Option -DTEMPLATE_PLACEHOLDER=96 in der platformio.ini setzen!
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include <time.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "hardware.h"
#include "version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

bool start_lp_core() { return false; }  // LP-Core kommt später....


// ============================================
// RTC Memory Struktur für Config-Werte
// ============================================
typedef struct {
    char hostname[32];
    char adminpass[32];
    struct {
        char ssid[32];
        char password[64];
    } wifi_credentials[2];  // Max 2 SSID/Passwort-Paare
    uint8_t wifi_count;  // Anzahl der gespeicherten Credentials (0, 1 oder 2)
    uint8_t wakeup_minutes;
    uint8_t transfer_minutes;
    float adc_voltage_offset;  // ADC-Offset-Korrektur in Volt (aus config.json oder hardware.h)
    char ntp_server[64];       // NTP-Server (aus config.json oder hardware.h)
    bool config_loaded;
} config_rtc_t;

RTC_DATA_ATTR config_rtc_t config_rtc = {
    .hostname = "gasometer2",
    .adminpass = "",
    .wifi_credentials = {{"", ""}, {"", ""}},
    .wifi_count = 0,
    .wakeup_minutes = DEFAULT_WAKEUP_INTERVAL_MIN,
    .transfer_minutes = DEFAULT_TRANSFER_INTERVAL_X * DEFAULT_WAKEUP_INTERVAL_MIN,
    .adc_voltage_offset = ADC_VOLTAGE_OFFSET,  // Default aus hardware.h
    .ntp_server = DEFAULT_NTP_SERVER,          // Default aus hardware.h
    .config_loaded = false
};
RTC_DATA_ATTR int wakeupCount = 0;  // Zählt nur Deep-Sleep-Wake-ups (nicht ESP.restart())
RTC_DATA_ATTR bool isPowerOn = false;
RTC_DATA_ATTR uint8_t ntp_sync_marker = 0;  // NTP-Sync-Status: 0=kein Sync, 1=letzter erfolgreich, 2+=fehlgeschlagene Versuche
RTC_DATA_ATTR uint32_t pulse_counter = 0;  // Puls-Zähler für LP-Core (aus NVS-Ring-Speicher geladen)
RTC_DATA_ATTR uint32_t ring_idx = RING_BUFFER_SIZE;  // Ring-Buffer-Index (im RTC-RAM, wird bei Power-On/ESP.restart() neu ermittelt)
RTC_DATA_ATTR uint32_t lp_core_running = 0;  // LP-Core Watchdog-Zähler (wird vom LP-Core regelmäßig erhöht)

// ============================================
// Globale Variablen
// ============================================
AsyncWebServer server(80);
bool littlefs_mounted = false;
bool server_started = false;  // Flag: Web-Server gestartet?

// Akku-Messwerte
uint32_t battery_adc_mv = 0;  // ADC-Wert in Millivolt (von analogReadMilliVolts - automatisch kalibriert)
float battery_voltage = 0.0f;
uint8_t battery_percent = 0;

// Web-Server Inaktivitäts-Timer
unsigned long last_web_activity = 0;  // Zeitpunkt der letzten Web-Server-Aktivität

// Deep-Sleep-Steuerung (zentralisiert in loop())
bool should_enter_deep_sleep = false;
const char* deep_sleep_reason = NULL;

// ============================================
// ADC Initialisierung (Arduino analogReadMilliVolts - automatisch kalibriert)
// ============================================
esp_err_t init_adc() {
    // analogReadMilliVolts() verwendet automatisch die richtige Attenuation
    // und ist bereits kalibriert - keine manuelle Konfiguration nötig
    // Pin-Modus wird automatisch gesetzt
    pinMode(BATTERY_ADC_PIN, INPUT);
    return ESP_OK;
}

// ============================================
// ADC-Messung mit Median-Filter (in Millivolt)
// ============================================
uint32_t read_adc_median_mv() {
    uint32_t adc_values[ADC_SAMPLE_COUNT];
    
    // Insertion Sort beim Einlesen (effizienter für kleine Arrays)
    for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
        // analogReadMilliVolts() gibt direkt Millivolt zurück (kalibriert)
        uint32_t mv = analogReadMilliVolts(BATTERY_ADC_PIN);
        
        // Einfügen und gleichzeitig sortieren (Insertion Sort)
        int j = i;
        while (j > 0 && adc_values[j - 1] > mv) {
            adc_values[j] = adc_values[j - 1];
            j--;
        }
        adc_values[j] = mv;
        
        delay(10);
    }
    
    // Median zurückgeben (mittlerer Wert im sortierten Array)
    return adc_values[ADC_SAMPLE_COUNT / 2];
}

// ============================================
// LittleFS Mount (einmalig)
// ============================================
bool mount_littlefs() {
    if (littlefs_mounted) {
        return true;
    }
    
    // LittleFS mit explizitem Partitionsnamen mounten
    // Parameter: formatOnFail, basePath, maxOpenFiles, partitionLabel
    if (!LittleFS.begin(true, "", 10, "storage")) {
        Serial.println("LittleFS Mount fehlgeschlagen");
        return false;
    }
    
    littlefs_mounted = true;
    return true;
}

// ============================================
// NVS-Ring-Speicher: Höchsten Puls-Zähler-Wert finden
// ============================================
uint32_t find_max_pulse_and_index_from_nvs(uint32_t* out_max_index) {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    uint32_t max_pulse = 0;
    uint32_t max_index = 0;  // Index des höchsten Wertes
    uint32_t found_count = 0;
    
    // NVS-Namespace aus spezieller Partition öffnen (read-only)
    err = nvs_open_from_partition(NVS_PARTITION_PULSE, NVS_NAMESPACE_PULSE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        Serial.printf("NVS-Namespace '%s' in Partition '%s' konnte nicht geöffnet werden: %s\n", 
                     NVS_NAMESPACE_PULSE, NVS_PARTITION_PULSE, esp_err_to_name(err));
        if (out_max_index != nullptr) {
            *out_max_index = 0;
        }
        return 0;  // Keine Daten vorhanden
    }
    
    Serial.printf("Durchsuche NVS-Ring-Speicher '%s' nach höchstem Puls-Zähler-Wert...\n", 
                 NVS_NAMESPACE_PULSE);
    
    // Alle möglichen Keys durchsuchen (p_0 bis p_11999)
    for (uint32_t i = 0; i < RING_BUFFER_SIZE; i++) {
        char key[MAX_KEY_LENGTH];
        snprintf(key, sizeof(key), "%s%lu", NVS_KEY_PREFIX, i);
        
        uint32_t pulse_value = 0;
        
        // Versuche, den Wert zu lesen
        err = nvs_get_u32(nvs_handle, key, &pulse_value);
        if (err == ESP_OK) {
            // Wert gefunden
            found_count++;
            if (pulse_value > max_pulse) {
                max_pulse = pulse_value;
                max_index = i;  // Index des höchsten Wertes speichern
            }
        }
        // ESP_ERR_NVS_NOT_FOUND ignorieren (Key existiert nicht)
    }
    
    nvs_close(nvs_handle);
    
    if (out_max_index != nullptr) {
        *out_max_index = max_index;
    }
    
    if (found_count > 0) {
        if (max_pulse > 0) {
            // Echte Daten gefunden (Werte > 0)
            Serial.printf("NVS-Ring-Speicher: %lu Einträge gefunden, höchster Wert: %lu (Index: %lu)\n", 
                         found_count, max_pulse, max_index);
        } else {
            // Nur Initialisierungsreste gefunden (alle Werte = 0)
            Serial.printf("NVS-Ring-Speicher: %lu Initialisierungsreste gefunden (alle 0) - keine echten Daten\n", 
                         found_count);
            max_pulse = 0;  // Als "keine Daten" behandeln
        }
    } else {
        Serial.println("NVS-Ring-Speicher: Keine Einträge gefunden");
    }
    
    return max_pulse;
}

uint32_t find_max_pulse_from_nvs() {
    uint32_t dummy_index;
    return find_max_pulse_and_index_from_nvs(&dummy_index);
}

// ============================================
// Pulse-NVS-Partition initialisieren (minimal, nur für Ring-Speicher)
// ============================================
bool init_pulse_nvs_minimal() {
    // Prüfe, ob bereits initialisiert
    esp_err_t err = nvs_flash_init_partition(NVS_PARTITION_PULSE);
    if (err == ESP_OK) {
        return true;  // Bereits initialisiert
    }
    
    // Versuche zu initialisieren (ohne Löschung, da wir nur lesen/schreiben wollen)
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Serial.printf("Pulse-NVS-Partition '%s' benötigt Initialisierung...\n", NVS_PARTITION_PULSE);
        // Bei Wake-up: Versuche erneut zu initialisieren (ohne Löschung)
        err = nvs_flash_init_partition(NVS_PARTITION_PULSE);
    }
    
    if (err != ESP_OK) {
        Serial.printf("Pulse-NVS-Initialisierung fehlgeschlagen: %s\n", esp_err_to_name(err));
        return false;
    }
    
    Serial.printf("Pulse-NVS-Partition '%s' erfolgreich initialisiert\n", NVS_PARTITION_PULSE);
    return true;
}

// ============================================
// RTC pulse_counter: In Ring-Speicher schreiben (bei ESP.restart(), Akku-Low, USB)
// ============================================
bool write_pulse_counter_to_ring_buffer() {
    // WICHTIG: Stelle sicher, dass Pulse-NVS initialisiert ist
    if (!init_pulse_nvs_minimal()) {
        Serial.println("FEHLER: Pulse-NVS konnte nicht initialisiert werden → kein Schreiben möglich");
        return false;
    }
    
    // Prüfung: Nur schreiben, wenn pulse_counter > 0
    if (pulse_counter == 0) {
        Serial.println("pulse_counter ist 0 → Keine Ring-Speicher-Schreibung nötig");
        return true;  // Kein Fehler, einfach nichts zu speichern
    }
    
    // Prüfung: Nur schreiben, wenn pulse_counter > max_pulse aus Ring-Speicher
    uint32_t max_pulse = find_max_pulse_from_nvs();
    if (pulse_counter <= max_pulse) {
        Serial.printf("pulse_counter nicht gespeichert: RTC=%lu <= Ring-Speicher-Max=%lu\n", 
                     pulse_counter, max_pulse);
        return true;  // Kein Fehler, einfach nichts zu speichern
    }
    
    // Ring-Speicher öffnen
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open_from_partition(NVS_PARTITION_PULSE, NVS_NAMESPACE_PULSE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        Serial.printf("Fehler beim Öffnen von Ring-Speicher für pulse_counter: %s\n", esp_err_to_name(err));
        return false;
    }
    
    // ring_idx aus RTC-RAM verwenden (nicht aus NVS lesen!)
    // WICHTIG: ring_idx wird im RTC-RAM gehalten, um Wear-Leveling-Hotspot zu vermeiden
    if (ring_idx >= RING_BUFFER_SIZE) {
        // ring_idx ist ungültig (z.B. nach Power-On, aber noch nicht initialisiert)
        Serial.println("WARNUNG: ring_idx ist ungültig! Sollte beim Boot initialisiert werden.");
        nvs_close(nvs_handle);
        return false;
    }
    
    // pulse_counter an Position ring_idx schreiben
    char key[MAX_KEY_LENGTH];
    snprintf(key, sizeof(key), "%s%lu", NVS_KEY_PREFIX, ring_idx);
    err = nvs_set_u32(nvs_handle, key, pulse_counter);
    if (err != ESP_OK) {
        Serial.printf("Fehler beim Schreiben von pulse_counter in Ring-Speicher: %s\n", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    // Ring-Index inkrementieren (modulo RING_BUFFER_SIZE) - NUR im RTC-RAM!
    // NICHT mehr in NVS speichern (vermeidet Wear-Leveling-Hotspot)
    ring_idx = (ring_idx + 1) % RING_BUFFER_SIZE;
    
    // Änderungen committen
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        Serial.printf("Fehler beim Commit von Ring-Speicher: %s\n", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    nvs_close(nvs_handle);
    
    Serial.printf("pulse_counter in Ring-Speicher geschrieben: %lu (Position: %lu, nächster Index: %lu)\n", 
                 pulse_counter, (ring_idx == 0 ? RING_BUFFER_SIZE - 1 : ring_idx - 1), ring_idx);
    return true;
}


// FreeRTOS Task für LP-Core Watchdog
void lp_core_watchdog_task(void *parameter) {
    uint32_t last_lp_core_value = 0;
    uint8_t retry_count = 0;
    const uint8_t MAX_RETRIES = 3;
    
    Serial.println("LP-Core Watchdog Task gestartet");
    
    // Initialisiere last_lp_core_value mit aktuellem Wert
    last_lp_core_value = lp_core_running;
    if (last_lp_core_value > 0) {
        Serial.printf("LP-Core läuft bereits (Zähler: %lu)\n", last_lp_core_value);
    }
    
    // Kombinierte Start- und Watchdog-Schleife
    // Wenn lp_core_running == 0 ODER Counter erhöht sich nicht → versuche LP-Core zu starten
    // Wenn nach MAX_RETRIES immer noch nicht erfolgreich → Task beenden
    while (1) {
        // Prüfe ob LP-Core läuft (lp_core_running == 0 bedeutet: nicht gestartet oder gestoppt)
        if (lp_core_running == 0) {
            // LP-Core läuft nicht → versuche zu starten
            retry_count++;
            Serial.printf("LP-Core läuft nicht (lp_core_running == 0) → Starte LP-Core... (Versuch %d/%d)\n", 
                         retry_count, MAX_RETRIES);
            
            if (retry_count >= MAX_RETRIES) {
                Serial.printf("FEHLER: LP-Core konnte nach %d Versuchen nicht gestartet werden. Watch-Dog-Task beendet!\n", MAX_RETRIES);
                vTaskDelete(NULL);
                return;
            }
            
            // Versuche LP-Core zu starten
            if (start_lp_core()) {
                // start_lp_core() gab true zurück - warte auf Watchdog-Timeout und prüfe dann
                last_lp_core_value = lp_core_running;
                Serial.printf("LP-Core Start aufgerufen (Zähler: %lu) - warte auf Watchdog-Timeout für Prüfung...\n", last_lp_core_value);
            } else {
                // start_lp_core() gab false zurück
                Serial.println("FEHLER: LP-Core Start fehlgeschlagen (start_lp_core() gab false zurück)!");
                // Kurz warten und erneut versuchen
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        } else {
            // LP-Core sollte laufen (lp_core_running > 0) → Watchdog-Prüfung
            // Warte LP_CORE_WATCHDOG_MS bevor Prüfung (gibt LP-Core Zeit, Counter zu erhöhen)
            vTaskDelay(pdMS_TO_TICKS(LP_CORE_WATCHDOG_MS));
            
            uint32_t current_lp_core_value = lp_core_running;
            
            // Prüfe ob Zähler sich erhöht hat
            if (current_lp_core_value == last_lp_core_value) {
                // Zähler hat sich nicht erhöht → LP-Core läuft nicht mehr!
                Serial.printf("WARNUNG: LP-Core Watchdog-Timeout! (Zähler: %lu, erwartet: > %lu)\n", 
                             current_lp_core_value, last_lp_core_value);
                Serial.println("Setze lp_core_running auf 0 und versuche LP-Core neu zu starten...");
                
                // Setze lp_core_running auf 0, damit wir in die Start-Schleife kommen
                lp_core_running = 0;
                retry_count = 0;  // Reset Retry-Counter für Neustart-Versuche
                continue;  // Gehe zurück in Start-Schleife
            } else {
                // Zähler hat sich erhöht → LP-Core läuft korrekt
                Serial.printf("LP-Core Watchdog OK (Zähler: %lu → %lu)\n", 
                             last_lp_core_value, current_lp_core_value);
                last_lp_core_value = current_lp_core_value;
                retry_count = 0;  // Reset Retry-Counter bei erfolgreichem Betrieb
            }
        }
    }
}

// ============================================
// NVS-Ring-Speicher initialisieren (alle Werte auf 0)
// ============================================
bool init_pulse_ring_nvs() {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    bool success = true;
    
    // NVS-Namespace aus spezieller Partition öffnen (read-write)
    err = nvs_open_from_partition(NVS_PARTITION_PULSE, NVS_NAMESPACE_PULSE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        Serial.printf("NVS-Namespace '%s' in Partition '%s' konnte nicht geöffnet werden: %s\n", 
                     NVS_NAMESPACE_PULSE, NVS_PARTITION_PULSE, esp_err_to_name(err));
        return false;
    }
    
    Serial.println("Initialisiere NVS-Ring-Speicher (Metadaten)...");
    
    // Alte Initialisierungsreste löschen (falls vorhanden)
    // Wir löschen alle p_* Keys, die möglicherweise von einem vorherigen fehlgeschlagenen Versuch stammen
    // Dies ist sicher, da wir Lazy Initialization verwenden und nur Metadaten speichern
    Serial.println("Lösche eventuelle Initialisierungsreste...");
    for (uint32_t i = 0; i < RING_BUFFER_SIZE; i++) {
        char key[MAX_KEY_LENGTH];
        snprintf(key, sizeof(key), "%s%lu", NVS_KEY_PREFIX, i);
        // Versuche Key zu löschen (ignoriere Fehler, wenn Key nicht existiert)
        nvs_erase_key(nvs_handle, key);
    }
    
    // WICHTIG: Wir initialisieren NICHT alle 12.000 Einträge auf einmal!
    // Stattdessen verwenden wir "Lazy Initialization":
    // - Einträge werden erst beim tatsächlichen Schreiben angelegt
    // - Beim Lesen: Wenn ein Eintrag nicht existiert, ist er implizit 0
    // Dies spart NVS-Speicherplatz und vermeidet ESP_ERR_NVS_NOT_ENOUGH_SPACE
    
    // WICHTIG: Ring-Index wird NICHT mehr in NVS gespeichert!
    // Stattdessen wird ring_idx im RTC-RAM gehalten (vermeidet Wear-Leveling-Hotspot)
    // ring_idx wird beim Boot (Power-On/ESP.restart()) aus Ring-Speicher ermittelt
    
    // Versionsnummer setzen
    uint32_t version = RING_BUFFER_VERSION;
    err = nvs_set_u32(nvs_handle, NVS_KEY_VERSION, version);
    if (err != ESP_OK) {
        Serial.printf("FEHLER beim Setzen der Versionsnummer: %s\n", esp_err_to_name(err));
        success = false;
    }
    
    // Änderungen committen
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        Serial.printf("FEHLER beim Committen der NVS-Änderungen: %s\n", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    nvs_close(nvs_handle);
    
    if (success) {
        Serial.printf("NVS-Ring-Speicher erfolgreich initialisiert (Version %lu, Timestamp: %lu)\n", 
                     version, version);
    } else {
        Serial.println("WARNUNG: NVS-Ring-Speicher-Initialisierung mit Fehlern abgeschlossen!");
    }
    
    return success;
}

// ============================================
// NVS-Ring-Speicher: Versionsnummer prüfen und ggf. initialisieren
// ============================================
bool check_and_init_pulse_ring_nvs() {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    // NVS-Namespace aus spezieller Partition öffnen (read-only)
    err = nvs_open_from_partition(NVS_PARTITION_PULSE, NVS_NAMESPACE_PULSE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        // Namespace existiert nicht → Initialisierung erforderlich
        Serial.printf("NVS-Namespace '%s' in Partition '%s' existiert nicht → Initialisierung erforderlich\n", 
                     NVS_NAMESPACE_PULSE, NVS_PARTITION_PULSE);
        return init_pulse_ring_nvs();
    }
    
    // Versionsnummer lesen
    uint32_t stored_version = 0;
    err = nvs_get_u32(nvs_handle, NVS_KEY_VERSION, &stored_version);
    nvs_close(nvs_handle);
    
    if (err != ESP_OK) {
        // Versionsnummer existiert nicht → Initialisierung erforderlich
        Serial.println("Versionsnummer nicht gefunden → Initialisierung erforderlich");
        return init_pulse_ring_nvs();
    }
    
    if (stored_version != RING_BUFFER_VERSION) {
        // Versionsnummer stimmt nicht überein → Initialisierung erforderlich
        Serial.printf("Versionsnummer stimmt nicht überein (gespeichert: %lu, erwartet: %lu) → Initialisierung erforderlich\n",
                     stored_version, RING_BUFFER_VERSION);
        return init_pulse_ring_nvs();
    }
    
    // Versionsnummer stimmt → keine Initialisierung nötig
    Serial.printf("NVS-Ring-Speicher Version %lu ist gültig → keine Initialisierung nötig\n", stored_version);
    return true;
}

// ============================================
// NVS-Partition initialisieren (Hilfsfunktion)
// ============================================
// Initialisiert eine NVS-Partition (Standard oder Pulse)
// WICHTIG: nvs_flash_init() ist idempotent - wenn bereits initialisiert, passiert nichts (keine Wear!)
// Bei Power-On: Partition wird gelöscht und neu initialisiert (falls nötig)
// Bei Deep-Sleep-Wake-up: Versucht erneut zu initialisieren ohne Löschung (NVS sollte noch vorhanden sein)
bool init_nvs_partition(const char* partition_name, bool is_power_on, bool is_standard_nvs) {
    esp_err_t err;
    
    // Initialisierung versuchen (Standard-NVS oder Pulse-NVS)
    if (is_standard_nvs) {
        err = nvs_flash_init();
    } else {
        err = nvs_flash_init_partition(partition_name);
    }
    
    // Prüfen, ob Neuinitialisierung erforderlich ist
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (is_power_on) {
            // Bei Power-On: Partition löschen und neu initialisieren
            Serial.printf("%s muss neu initialisiert werden...\n", partition_name);
            esp_err_t erase_err = is_standard_nvs ? 
                nvs_flash_erase() : 
                nvs_flash_erase_partition(partition_name);
            if (erase_err == ESP_OK) {
                err = is_standard_nvs ? 
                    nvs_flash_init() : 
                    nvs_flash_init_partition(partition_name);
            } else {
                Serial.printf("%s-Löschung fehlgeschlagen: %s\n", partition_name, esp_err_to_name(erase_err));
            }
        } else {
            // Bei Deep-Sleep-Wake-up: Versuche erneut ohne Löschung (NVS sollte noch vorhanden sein)
            Serial.printf("WARNUNG: %s benötigt Neuinitialisierung!\n", partition_name);
            Serial.println("Versuche erneut zu initialisieren (ohne Löschung)...");
            err = is_standard_nvs ? 
                nvs_flash_init() : 
                nvs_flash_init_partition(partition_name);
        }
    }
    
    // Ergebnis prüfen
    if (err != ESP_OK) {
        Serial.printf("%s-Initialisierung fehlgeschlagen: %s\n", partition_name, esp_err_to_name(err));
        return false;
    }
    Serial.printf("%s erfolgreich initialisiert\n", partition_name);
    return true;
}

// ============================================
// NVS-Partitionen initialisieren
// ============================================
// Initialisiert beide NVS-Partitionen (Standard und Pulse)
// Wird bei Power-On und Deep-Sleep-Wake-up aufgerufen
void init_nvs_partitions(bool is_power_on) {
    init_nvs_partition("Standard-NVS", is_power_on, true);
    init_nvs_partition(NVS_PARTITION_PULSE, is_power_on, false);
}

// ============================================
// ring_idx und pulse_counter initialisieren (kombiniert)
// ============================================
// Initialisiert ring_idx und pulse_counter aus RTC-RAM oder NVS-Ring-Speicher
// ring_idx: Ring-Buffer-Index (im RTC-RAM, wird bei Power-On/ESP.restart() neu ermittelt)
//           Bei Deep-Sleep-Wake-up sollte ring_idx noch im RTC-RAM vorhanden sein
// pulse_counter: Puls-Zähler (im RTC-RAM, wird bei Power-On/ESP.restart() aus Ring-Speicher geladen)
//                Bei Deep-Sleep-Wake-up ist RTC-RAM noch vorhanden und muss NICHT geladen werden
void init_ring_buffer_and_pulse_counter(bool is_power_on) {
    // ring_idx initialisieren
    // Bei Power-On oder wenn ring_idx ungültig: aus Ring-Speicher ermitteln
    if (is_power_on || ring_idx >= RING_BUFFER_SIZE) {
        uint32_t max_index = 0;
        uint32_t max_pulse = find_max_pulse_and_index_from_nvs(&max_index);
        if (max_pulse > 0) {
            // Nächster freier Slot: (max_index + 1) % RING_BUFFER_SIZE
            ring_idx = (max_index + 1) % RING_BUFFER_SIZE;
            Serial.printf("ring_idx aus Ring-Speicher: %lu (höchster Index: %lu)\n", ring_idx, max_index);
        } else {
            ring_idx = 0;  // Erster Slot (keine Daten vorhanden)
            Serial.println("ring_idx auf 0 gesetzt (keine Daten im Ring-Speicher)");
        }
    } else {
        // Bei Deep-Sleep-Wake-up: ring_idx aus RTC-RAM übernehmen
        Serial.printf("ring_idx aus RTC-RAM: %lu\n", ring_idx);
    }
    
    // pulse_counter initialisieren
    // Beim ersten Boot (Power-On) oder nach ESP.restart(): pulse_counter aus Ring-Speicher laden
    // WICHTIG: RTC-RAM ist bei Power-On/ESP.restart() leer (pulse_counter == 0)
    // Bei Deep-Sleep-Wake-up ist RTC-RAM noch vorhanden und muss NICHT geladen werden
    if (pulse_counter == 0) {
        uint32_t max_index = 0;
        uint32_t max_pulse = find_max_pulse_and_index_from_nvs(&max_index);
        if (max_pulse > 0) {
            pulse_counter = max_pulse;
            Serial.printf("pulse_counter aus Ring-Speicher: %lu\n", pulse_counter);
        } else {
            pulse_counter = 0;
            Serial.println("pulse_counter auf 0 initialisiert (keine Ring-Speicher-Daten)");
        }
    } else {
        // RTC-RAM noch vorhanden (bei Deep-Sleep-Wake-up)
        Serial.printf("pulse_counter aus RTC-RAM: %lu", pulse_counter);
        if (!is_power_on) {
            Serial.println(" (RTC-RAM behält Daten bei Deep-Sleep-Wake-up)");
        } else {
            Serial.println();
        }
    }
}

// ============================================
// Ressourcen sauber beenden (Web-Server, WiFi, LittleFS)
// ============================================
void shutdown_resources() {
    Serial.println("Schließe Ressourcen...");
    
    // 1. mDNS stoppen
    MDNS.end();
    Serial.println("mDNS gestoppt");
    delay(50);
    
    // 2. Web-Server stoppen
    if (server_started) {
        server.end();
        server_started = false;  // Flag zurücksetzen
        Serial.println("Web-Server gestoppt");
        delay(100);  // Kurze Verzögerung für sauberes Schließen
    }
    
    // 3. WiFi trennen und deaktivieren
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect(true);  // true = WiFi komplett deaktivieren
        Serial.println("WiFi getrennt und deaktiviert");
        delay(100);
    }
    WiFi.mode(WIFI_OFF);
    
    // 4. LittleFS unmounten (sichert alle ausstehenden Schreibvorgänge)
    if (littlefs_mounted) {
        LittleFS.end();
        littlefs_mounted = false;
        Serial.println("LittleFS unmounted");
        delay(50);
    }
    
    Serial.println("Alle Ressourcen freigegeben");
    Serial.flush();
    delay(200);  // Pause, damit Serial-Output gesendet wird
}

// ============================================
// Berechne nächsten Timer-Wake-up-Zeitpunkt (Cron-ähnlich)
// ============================================
uint64_t calculate_next_wakeup_timer() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("WARNUNG: Keine Zeit-Synchronisation → verwende Fallback-Intervall");
        return config_rtc.wakeup_minutes * 60 * 1000000ULL;  // Mikrosekunden
    }
    
    // 1. Ist-UNIX_EPOCH ermitteln
    time_t current_time = mktime(&timeinfo);
    
    // 2. Target-UNIX_EPOCH = Ist-UNIX_EPOCH + WAKEUP_BUFFER_SEC
    time_t target_time = current_time + WAKEUP_BUFFER_SEC;
    
    // 3. Intervall in Sekunden (z.B. 10 Minuten = 600 Sekunden)
    int interval_seconds = config_rtc.wakeup_minutes * 60;
    
    // 4. Auf nächste Intervall-Grenze aufrunden (Vielfaches von interval_seconds)
    // Beispiel: target_time = 12345, interval = 600 → next_wakeup = 12600
    time_t next_wakeup_time = ((target_time / interval_seconds) + 1) * interval_seconds;
    
    // 5. Wake-Interval = Target-UNIX_EPOCH - Ist-UNIX_EPOCH
    int seconds_until_wakeup = (int)(next_wakeup_time - current_time);
    
    // 6. Zur Textausgabe: Target-UNIX_EPOCH in Time-struct wandeln
    struct tm next_wakeup_tm;
    localtime_r(&next_wakeup_time, &next_wakeup_tm);
    Serial.printf("Aktuelle Zeit: %02d:%02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    Serial.printf("Nächster Wake-up: %02d:%02d:00 (in %d Sekunden = %.2f Minuten)\n", 
                 next_wakeup_tm.tm_hour, next_wakeup_tm.tm_min, seconds_until_wakeup, (float)seconds_until_wakeup / 60.0f);
    
    // Konvertiere zu Mikrosekunden für esp_sleep_enable_timer_wakeup()
    return (uint64_t)seconds_until_wakeup * 1000000ULL;
}

// ============================================
// Deep-Sleep mit GPIO- und Timer-Wake-up konfigurieren
// ============================================
// enable_timer: true = Timer-Wake-up aktivieren, false = nur GPIO-Wake-up (Taster)
void enter_deep_sleep_with_gpio_and_timer_wakeup(bool enable_timer = true) {
    Serial.println("Konfiguriere Deep-Sleep mit GPIO-Wake-up (Taster A)...");
    
    // Taster A (BUTTON_A_GPIO) als Wake-up-Source konfigurieren
    // ESP32C6 verwendet esp_sleep_enable_gpio_wakeup() statt esp_sleep_enable_ext0_wakeup()
    gpio_num_t gpio_num = (gpio_num_t)BUTTON_A_GPIO;
    
    // GPIO-Pin-Modus aus hardware.h bestimmen
    uint8_t gpio_mode = BUTTON_A_GPIO_MODE;
    gpio_int_type_t wakeup_level;
    const char* level_name;
    
    // WICHTIG: ESP32C6 unterstützt NUR Level-Mode, NICHT Edge-Mode!
    // Level-Mode: Weckt auf, wenn GPIO im angegebenen Level ist
    // Daher muss GPIO vor Deep-Sleep im "nicht-gedrückt" Zustand sein
    
    // Prüfen, ob BUTTON_A_GPIO RTC-fähig ist (für Deep-Sleep-Wake-up)
    // Laut ESP-IDF-Dokumentation: Nur GPIOs 0-7 haben RTC-Funktionalität
    bool is_rtc_gpio = (gpio_num <= 7);
    bool gpio_wakeup_configured = false;
    
    if (is_rtc_gpio) {
        // RTC-GPIO verwenden (empfohlen für Deep-Sleep-Wake-up)
        Serial.printf("GPIO%d ist RTC-fähig → konfiguriere GPIO-Wake-up\n", gpio_num);
        
        // RTC-GPIO initialisieren
        rtc_gpio_init(gpio_num);
        rtc_gpio_set_direction(gpio_num, RTC_GPIO_MODE_INPUT_ONLY);
        
        // Level basierend auf GPIO-Modus bestimmen
        if (gpio_mode == INPUT_PULLUP) {
            // INPUT_PULLUP: Taster zieht auf LOW → Wake-up bei LOW-Level
            wakeup_level = GPIO_INTR_LOW_LEVEL;
            level_name = "LOW LEVEL (Taster gedrückt = LOW)";
            rtc_gpio_pullup_dis(gpio_num);
            rtc_gpio_pulldown_dis(gpio_num);
            rtc_gpio_pullup_en(gpio_num);  // RTC-GPIO Pull-Up aktivieren
        } else if (gpio_mode == INPUT_PULLDOWN) {
            // INPUT_PULLDOWN: Taster zieht auf HIGH → Wake-up bei HIGH-Level
            wakeup_level = GPIO_INTR_HIGH_LEVEL;
            level_name = "HIGH LEVEL (Taster gedrückt = HIGH)";
            rtc_gpio_pullup_dis(gpio_num);
            rtc_gpio_pulldown_dis(gpio_num);
            rtc_gpio_pulldown_en(gpio_num);  // RTC-GPIO Pull-Down aktivieren
        } else {
            // INPUT (ohne Pull): Standardmäßig LOW-Level annehmen
            wakeup_level = GPIO_INTR_LOW_LEVEL;
            level_name = "LOW LEVEL (Default)";
            rtc_gpio_pullup_dis(gpio_num);
            rtc_gpio_pulldown_dis(gpio_num);
        }
        
        // WICHTIG: RTC_PERIPH Domain konfigurieren
        // Wenn RTC_PERIPH ausgeschaltet ist, wird HOLD automatisch verwendet
        // Wenn RTC_PERIPH eingeschaltet ist, funktionieren Pull-Ups/Pull-Downs normal
        // Für Deep-Sleep empfohlen: RTC_PERIPH ausschalten, HOLD wird automatisch verwendet
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
        Serial.println("RTC_PERIPH Domain ausgeschaltet → HOLD wird automatisch verwendet");
        
        // WICHTIG: Kurze Verzögerung, damit Pull-Up/Pull-Down stabilisiert
        delay(50);
        
        // Aktuellen GPIO-Zustand prüfen (für Debugging und Warnung)
        int gpio_state = rtc_gpio_get_level(gpio_num);
        Serial.printf("BUTTON_A_GPIO aktueller Zustand: %s\n", gpio_state ? "HIGH" : "LOW");
        
        // WICHTIG: Bei Level-Mode weckt ESP32C6 sofort, wenn GPIO bereits im Wake-up-Level ist!
        // Daher prüfen und warnen, falls GPIO bereits im Wake-up-Level ist
        if ((wakeup_level == GPIO_INTR_LOW_LEVEL && gpio_state == 0) ||
            (wakeup_level == GPIO_INTR_HIGH_LEVEL && gpio_state == 1)) {
            Serial.println("WARNUNG: BUTTON_A_GPIO ist bereits im Wake-up-Level! System würde sofort wecken.");
            Serial.println("Stelle sicher, dass Taster nicht gedrückt ist, bevor Deep-Sleep startet!");
            return;  // Deep-Sleep abbrechen
        }
        
        // GPIO-Wake-up aktivieren (ESP32C6 unterstützt nur gpio_wakeup, nicht ext0/ext1)
        esp_err_t ret = esp_sleep_enable_gpio_wakeup();
        if (ret != ESP_OK) {
            Serial.printf("FEHLER: GPIO-Wake-up-Konfiguration fehlgeschlagen: %s\n", esp_err_to_name(ret));
            Serial.println("Deep-Sleep wird ohne GPIO-Wake-up gestartet!");
        } else {
            // GPIO als Wake-up-Source setzen: Level basierend auf GPIO-Modus
            // ESP32C6 unterstützt NUR Level-Mode (LOW_LEVEL oder HIGH_LEVEL)
            ret = gpio_wakeup_enable(gpio_num, wakeup_level);
            if (ret != ESP_OK) {
                Serial.printf("FEHLER: gpio_wakeup_enable fehlgeschlagen: %s\n", esp_err_to_name(ret));
                Serial.printf("Fehler-Code: %s\n", esp_err_to_name(ret));
            } else {
                gpio_wakeup_configured = true;
                Serial.printf("GPIO-Wake-up konfiguriert: Taster A (BUTTON_A_GPIO) - %s\n", level_name);
                Serial.printf("GPIO-Modus: %s\n", 
                             gpio_mode == INPUT_PULLUP ? "INPUT_PULLUP" : 
                             (gpio_mode == INPUT_PULLDOWN ? "INPUT_PULLDOWN" : "INPUT"));
                Serial.println("HOLD-Funktion aktiv: Pull-Up/Pull-Down wird während Deep-Sleep gehalten");
            }
        }
    } else {
        // Nicht-RTC-Pin: GPIO-Wake-up nicht möglich
        Serial.printf("WARNUNG: GPIO%d ist NICHT RTC-fähig (nur GPIOs 0-7)\n", gpio_num);
        Serial.println("GPIO-Wake-up (Taster A) kann nicht konfiguriert werden!");
        Serial.println("Nur Timer-Wake-up möglich (falls aktiviert)");
    }
    
    // Timer-Wake-up berechnen und aktivieren (nur wenn enable_timer == true)
    // WICHTIG: Bei kritischer Akku-Spannung (<= BATTERY_VOLTAGE_PROTECTION) wird Timer deaktiviert,
    // um weiteren Akku-Verschleiß durch automatische Wake-ups zu vermeiden
    bool timer_activated = false;
    uint64_t wakeup_time_us = 0;
    struct tm next_wakeup_tm = {0};
    
    if (enable_timer) {
        wakeup_time_us = calculate_next_wakeup_timer();
        
        if (wakeup_time_us > 0) {
            esp_err_t ret = esp_sleep_enable_timer_wakeup(wakeup_time_us);
            if (ret != ESP_OK) {
                Serial.printf("FEHLER: Timer-Wake-up-Konfiguration fehlgeschlagen: %s\n", esp_err_to_name(ret));
            } else {
                timer_activated = true;
                // Berechne nächste Wake-up-Zeit für Ausgabe
                struct tm timeinfo;
                if (getLocalTime(&timeinfo)) {
                    time_t current_time = mktime(&timeinfo);
                    time_t next_wakeup_time = current_time + (wakeup_time_us / 1000000ULL);
                    localtime_r(&next_wakeup_time, &next_wakeup_tm);
                }
            }
        }
    } else {
        Serial.println("Timer-Wake-up DEAKTIVIERT (nur GPIO-Wake-up aktiv)");
        Serial.println("Nur manueller Wake-up über Taster A möglich");
    }
    
    // Prüfen, ob mindestens eine Wake-up-Quelle konfiguriert wurde
    // WICHTIG: Ohne Wake-up-Quelle würde ESP32C6 nicht mehr aufwachen!
    if (!gpio_wakeup_configured && !timer_activated) {
        Serial.println("FEHLER: Keine Wake-up-Quelle konfiguriert!");
        Serial.println("Deep-Sleep wird ABGEBROCHEN - System bleibt aktiv");
        Serial.println("Mögliche Ursachen:");
        if (!is_rtc_gpio) {
            Serial.println("  - GPIO ist nicht RTC-fähig (nur GPIOs 0-7)");
        }
        if (!enable_timer) {
            Serial.println("  - Timer-Wake-up ist deaktiviert (Akku-Schutz)");
        } else if (!timer_activated) {
            Serial.println("  - Timer-Wake-up konnte nicht aktiviert werden");
        }
        return;  // Funktion beenden, Ressourcen bleiben aktiv
    }
    
    // LED ausschalten (HP-Core geht in Deep-Sleep)
    digitalWrite(LED_BUILTIN_GPIO, LED_OFF);
    Serial.println("Interne LED ausgeschaltet (Deep-Sleep)");
    
    // Ressourcen sauber beenden (nur wenn Wake-up konfiguriert wurde)
    shutdown_resources();
    
    // Finale Deep-Sleep-Ausgabe (dynamisch basierend auf aktivierten Wake-up-Quellen)
    Serial.println("=== Gehe in Deep-Sleep ===");
    Serial.println("Wake-up möglich durch:");
    if (gpio_wakeup_configured) {
        Serial.println("  - Taster A (GPIO-Wake-up)");
    } else {
        Serial.println("  - Taster A: NICHT verfügbar (GPIO nicht RTC-fähig)");
    }
    if (timer_activated) {
        Serial.printf("  - Timer (Cron-Intervall): %02d:%02d:00 (in %llu Sekunden = %.2f Minuten)\n",
                     next_wakeup_tm.tm_hour, next_wakeup_tm.tm_min,
                     wakeup_time_us / 1000000ULL, (float)wakeup_time_us / 60000000.0f);
    } else {
        if (enable_timer) {
            Serial.println("  - Timer: FEHLER bei Aktivierung");
        } else {
            Serial.println("  - Timer: DEAKTIVIERT (Akku-Schutz)");
        }
    }
    Serial.flush();
    
    esp_deep_sleep_start();
    // Ab hier wird Code nicht mehr ausgeführt
}

// ============================================
// Config.json laden
// ============================================
bool load_config() {
    // Prüfen, ob Config bereits geladen ist (z.B. aus RTC-RAM nach Wake-up)
    if (config_rtc.config_loaded) {
        Serial.println("Config bereits geladen (aus RTC-RAM) → kein erneutes Laden nötig");
        return true;
    }
    
    if (!mount_littlefs()) {
        return false;
    }
    
    File configFile = LittleFS.open("/config.json", "r");
    if (!configFile) {
        Serial.println("config.json nicht gefunden");
        return false;
    }
    
    size_t size = configFile.size();
    if (size > 1024) {
        Serial.println("config.json zu groß");
        configFile.close();
        return false;
    }
    
    // JSON parsen
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, configFile);
    configFile.close();
    
    if (error) {
        Serial.printf("JSON Parse Fehler: %s\n", error.c_str());
        return false;
    }
    
    // Werte in RTC Memory speichern (mit Null-Terminierung)
    const char* hostname = doc["hostname"] | "gas-o-meter2";
    strncpy(config_rtc.hostname, hostname, sizeof(config_rtc.hostname) - 1);
    config_rtc.hostname[sizeof(config_rtc.hostname) - 1] = '\0';
    
    const char* adminpass = doc["adminpass"] | "";
    strncpy(config_rtc.adminpass, adminpass, sizeof(config_rtc.adminpass) - 1);
    config_rtc.adminpass[sizeof(config_rtc.adminpass) - 1] = '\0';
    
    // Wake-up Intervall: aus config.json oder Default aus hardware.h
    if (doc["wakeup_minutes"].is<uint8_t>()) {
        config_rtc.wakeup_minutes = doc["wakeup_minutes"].as<uint8_t>();
    } else {
        config_rtc.wakeup_minutes = DEFAULT_WAKEUP_INTERVAL_MIN;
    }
    
    // Transfer Intervall: aus config.json oder Default (DEFAULT_TRANSFER_INTERVAL_X * DEFAULT_WAKEUP_INTERVAL_MIN)
    if (doc["tarnsfer_minutes"].is<uint8_t>()) {  // Tippfehler in JSON beibehalten
        config_rtc.transfer_minutes = doc["tarnsfer_minutes"].as<uint8_t>();
    } else {
        // Default: DEFAULT_TRANSFER_INTERVAL_X * DEFAULT_WAKEUP_INTERVAL_MIN
        config_rtc.transfer_minutes = DEFAULT_TRANSFER_INTERVAL_X * DEFAULT_WAKEUP_INTERVAL_MIN;
    }
    
    // ADC-Offset: aus config.json oder Default aus hardware.h
    if (doc["adc_voltage_offset"].is<float>()) {
        config_rtc.adc_voltage_offset = doc["adc_voltage_offset"].as<float>();
    } else {
        config_rtc.adc_voltage_offset = ADC_VOLTAGE_OFFSET;
    }
    
    // NTP-Server: aus config.json oder Default aus hardware.h
    if (doc["ntp_server"].is<const char*>()) {
        const char* ntp_server = doc["ntp_server"] | DEFAULT_NTP_SERVER;
        strncpy(config_rtc.ntp_server, ntp_server, sizeof(config_rtc.ntp_server) - 1);
        config_rtc.ntp_server[sizeof(config_rtc.ntp_server) - 1] = '\0';
    } else {
        strncpy(config_rtc.ntp_server, DEFAULT_NTP_SERVER, sizeof(config_rtc.ntp_server) - 1);
        config_rtc.ntp_server[sizeof(config_rtc.ntp_server) - 1] = '\0';
    }
    
    // WiFi-Credentials laden (max. 2 Paare)
    config_rtc.wifi_count = 0;
    if (doc["wifiCredentials"].is<JsonArray>()) {
        JsonArray credentials = doc["wifiCredentials"].as<JsonArray>();
        uint8_t max_credentials = (credentials.size() > 2) ? 2 : credentials.size();
        
        for (uint8_t i = 0; i < max_credentials; i++) {
            if (credentials[i]["ssid"].is<const char*>() && credentials[i]["password"].is<const char*>()) {
                const char* ssid = credentials[i]["ssid"];
                const char* password = credentials[i]["password"];
                
                strncpy(config_rtc.wifi_credentials[i].ssid, ssid, sizeof(config_rtc.wifi_credentials[i].ssid) - 1);
                config_rtc.wifi_credentials[i].ssid[sizeof(config_rtc.wifi_credentials[i].ssid) - 1] = '\0';
                
                strncpy(config_rtc.wifi_credentials[i].password, password, sizeof(config_rtc.wifi_credentials[i].password) - 1);
                config_rtc.wifi_credentials[i].password[sizeof(config_rtc.wifi_credentials[i].password) - 1] = '\0';
                
                config_rtc.wifi_count++;
            }
        }
        
        if (credentials.size() > 2) {
            Serial.printf("WARNUNG: Mehr als 2 WiFi-Credentials gefunden, nur die ersten 2 werden verwendet\n");
        }
        Serial.printf("WiFi-Credentials geladen: %d Paar(e)\n", config_rtc.wifi_count);
    }
    
    config_rtc.config_loaded = true;
    
    Serial.println("Config.json erfolgreich geladen");
    return true;
}

// ============================================
// Config speichern (RTC-RAM und config.json)
// ============================================
// Rückgabewert: true = erfolgreich, false = Fehler
// wifi_credentials_changed wird auf true gesetzt, wenn WiFi-Credentials geändert wurden
bool save_config(JsonDocument& doc, bool* wifi_credentials_changed = nullptr) {
    if (wifi_credentials_changed != nullptr) {
        *wifi_credentials_changed = false;
    }
    
    if (!mount_littlefs()) {
        Serial.println("Fehler: LittleFS nicht gemountet");
        return false;
    }
    
    // Alte WiFi-Credentials speichern für Vergleich
    struct {
        uint8_t count;
        char ssid[2][32];
        char password[2][64];
    } old_wifi;
    old_wifi.count = config_rtc.wifi_count;
    for (uint8_t i = 0; i < config_rtc.wifi_count && i < 2; i++) {
        strncpy(old_wifi.ssid[i], config_rtc.wifi_credentials[i].ssid, sizeof(old_wifi.ssid[i]) - 1);
        old_wifi.ssid[i][sizeof(old_wifi.ssid[i]) - 1] = '\0';
        strncpy(old_wifi.password[i], config_rtc.wifi_credentials[i].password, sizeof(old_wifi.password[i]) - 1);
        old_wifi.password[i][sizeof(old_wifi.password[i]) - 1] = '\0';
    }
    
    // Werte in RTC Memory speichern
    if (doc["hostname"].is<const char*>()) {
        const char* hostname = doc["hostname"];
        strncpy(config_rtc.hostname, hostname, sizeof(config_rtc.hostname) - 1);
        config_rtc.hostname[sizeof(config_rtc.hostname) - 1] = '\0';
    }
    
    if (doc["adminpass"].is<const char*>()) {
        const char* adminpass = doc["adminpass"];
        strncpy(config_rtc.adminpass, adminpass, sizeof(config_rtc.adminpass) - 1);
        config_rtc.adminpass[sizeof(config_rtc.adminpass) - 1] = '\0';
    }
    
    if (doc["wakeup_minutes"].is<uint8_t>()) {
        config_rtc.wakeup_minutes = doc["wakeup_minutes"].as<uint8_t>();
    }
    
    if (doc["transfer_minutes"].is<uint8_t>()) {
        config_rtc.transfer_minutes = doc["transfer_minutes"].as<uint8_t>();
    }
    
    if (doc["adc_voltage_offset"].is<float>()) {
        config_rtc.adc_voltage_offset = doc["adc_voltage_offset"].as<float>();
    }
    
    if (doc["ntp_server"].is<const char*>()) {
        const char* ntp_server = doc["ntp_server"];
        strncpy(config_rtc.ntp_server, ntp_server, sizeof(config_rtc.ntp_server) - 1);
        config_rtc.ntp_server[sizeof(config_rtc.ntp_server) - 1] = '\0';
    }
    
    // WiFi-Credentials speichern (max. 2 Paare)
    config_rtc.wifi_count = 0;
    if (doc["wifiCredentials"].is<JsonArray>()) {
        JsonArray credentials = doc["wifiCredentials"].as<JsonArray>();
        uint8_t max_credentials = (credentials.size() > 2) ? 2 : credentials.size();
        
        for (uint8_t i = 0; i < max_credentials; i++) {
            if (credentials[i]["ssid"].is<const char*>() && credentials[i]["password"].is<const char*>()) {
                const char* ssid = credentials[i]["ssid"];
                const char* password = credentials[i]["password"];
                
                // Nur nicht-leere Sets speichern
                if (strlen(ssid) > 0 || strlen(password) > 0) {
                    strncpy(config_rtc.wifi_credentials[i].ssid, ssid, sizeof(config_rtc.wifi_credentials[i].ssid) - 1);
                    config_rtc.wifi_credentials[i].ssid[sizeof(config_rtc.wifi_credentials[i].ssid) - 1] = '\0';
                    
                    strncpy(config_rtc.wifi_credentials[i].password, password, sizeof(config_rtc.wifi_credentials[i].password) - 1);
                    config_rtc.wifi_credentials[i].password[sizeof(config_rtc.wifi_credentials[i].password) - 1] = '\0';
                    
                    config_rtc.wifi_count++;
                }
            }
        }
    }
    
    // Config in config.json speichern
    // WICHTIG: Wir müssen ein neues JSON-Dokument erstellen, da das eingehende doc
    // möglicherweise nicht alle Felder enthält (z.B. wenn nur WiFi-Credentials geändert wurden)
    File configFile = LittleFS.open("/config.json", "w");
    if (!configFile) {
        Serial.println("Fehler: config.json konnte nicht zum Schreiben geöffnet werden");
        return false;
    }
    
    // Neues JSON-Dokument erstellen mit allen Werten
    JsonDocument newDoc;
    newDoc["hostname"] = config_rtc.hostname;
    newDoc["adminpass"] = config_rtc.adminpass;
    newDoc["wakeup_minutes"] = config_rtc.wakeup_minutes;
    newDoc["tarnsfer_minutes"] = config_rtc.transfer_minutes;  // Tippfehler in JSON beibehalten
    newDoc["adc_voltage_offset"] = config_rtc.adc_voltage_offset;
    newDoc["ntp_server"] = config_rtc.ntp_server;
    
    // WiFi-Credentials als Array
    JsonArray wifiArray = newDoc["wifiCredentials"].to<JsonArray>();
    for (uint8_t i = 0; i < config_rtc.wifi_count; i++) {
        JsonObject cred = wifiArray.add<JsonObject>();
        cred["ssid"] = config_rtc.wifi_credentials[i].ssid;
        cred["password"] = config_rtc.wifi_credentials[i].password;
    }
    
    // JSON schreiben (mit Formatierung für bessere Lesbarkeit)
    serializeJsonPretty(newDoc, configFile);
    configFile.close();
    
    config_rtc.config_loaded = true;
    
    // Prüfen, ob WiFi-Credentials geändert wurden
    if (wifi_credentials_changed != nullptr) {
        bool changed = false;
        
        // Anzahl geändert?
        if (old_wifi.count != config_rtc.wifi_count) {
            changed = true;
        } else {
            // Einzelne Credentials vergleichen
            for (uint8_t i = 0; i < config_rtc.wifi_count && i < 2; i++) {
                if (strcmp(old_wifi.ssid[i], config_rtc.wifi_credentials[i].ssid) != 0 ||
                    strcmp(old_wifi.password[i], config_rtc.wifi_credentials[i].password) != 0) {
                    changed = true;
                    break;
                }
            }
        }
        
        *wifi_credentials_changed = changed;
        
        if (changed) {
            Serial.println("WiFi-Credentials wurden geändert");
        }
    }
    
    Serial.println("Config erfolgreich gespeichert (RTC-RAM und config.json)");
    return true;
}

// ============================================
// WiFi-Verbindung mit höchster RSSI
// ============================================
bool connect_wifi() {
    if (config_rtc.wifi_count == 0) {
        Serial.println("Keine WiFi-Credentials verfügbar");
        return false;
    }
    
    // WiFi initialisieren (konsistent für Power-On und Reboot)
    // WICHTIG: WiFi-Modus explizit setzen, um sicherzustellen, dass WiFi bereit ist
    WiFi.mode(WIFI_OFF);  // Zuerst komplett ausschalten
    delay(100);
    WiFi.mode(WIFI_STA);  // Dann Station-Modus aktivieren
    WiFi.disconnect();    // Bestehende Verbindungen trennen
    delay(100);
    
    // Hostname setzen (für DHCP-Server, damit dieser den Namen in DNS eintragen kann)
    WiFi.setHostname(config_rtc.hostname);
    Serial.printf("Hostname gesetzt: %s\n", config_rtc.hostname);
    
    // WiFi-Scan durchführen
    int n = WiFi.scanNetworks();
    if (n == 0) {
        Serial.println("Keine Netzwerke gefunden");
        return false;
    }
    
    // Bestes Netzwerk finden (höchste RSSI)
    int best_rssi = -1000;
    const char* best_ssid = NULL;
    const char* best_password = NULL;
    
    for (uint8_t i = 0; i < config_rtc.wifi_count; i++) {
        const char* ssid = config_rtc.wifi_credentials[i].ssid;
        const char* password = config_rtc.wifi_credentials[i].password;
        
        for (int j = 0; j < n; j++) {
            // SSID in temporären String kopieren für sicheren Vergleich
            String scanned_ssid = WiFi.SSID(j);
            if (scanned_ssid.equals(ssid)) {
                int rssi = WiFi.RSSI(j);
                if (rssi > best_rssi) {
                    best_rssi = rssi;
                    best_ssid = ssid;
                    best_password = password;
                }
            }
        }
    }
    
    if (best_ssid == NULL) {
        Serial.println("Kein bekanntes Netzwerk gefunden");
        return false;
    }
    
    // Verbindung herstellen
    Serial.printf("Verbinde mit: %s (RSSI: %d dBm)\n", best_ssid, best_rssi);
    WiFi.begin(best_ssid, best_password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi verbunden! IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    } else {
        Serial.println("WiFi-Verbindung fehlgeschlagen");
        return false;
    }
}

// ============================================
// NTP-Zeitsynchronisation
// ============================================
bool sync_ntp_time() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("NTP-Sync: WiFi nicht verbunden");
        // Fehlgeschlagen: Marker erhöhen (wenn > 0) oder bei 0 bleiben
        if (ntp_sync_marker > 0) {
            ntp_sync_marker++;
            if (ntp_sync_marker > 255) {
                ntp_sync_marker = 0;  // Nach 255 fehlgeschlagenen Versuchen auf 0 zurücksetzen
            }
        }
        return false;
    }
    
    Serial.printf("NTP-Sync: Verbinde mit %s...\n", config_rtc.ntp_server);
    
    // Zeitzone explizit auf UTC setzen (für Datenlogger wichtig - keine Sommer/Winterzeit-Sprünge!)
    setenv("TZ", "UTC", 1);
    tzset();
    
    // NTP-Konfiguration (UTC, keine DST-Offset)
    configTime(0, 0, config_rtc.ntp_server);
    
    // Warte auf Zeit-Synchronisation
    struct tm timeinfo;
    unsigned long start_time = millis();
    
    while (!getLocalTime(&timeinfo) && (millis() - start_time) < NTP_TIMEOUT_MS) {
        delay(100);
    }
    
    if (getLocalTime(&timeinfo)) {
        // Sync erfolgreich: Marker auf 1 setzen
        ntp_sync_marker = 1;
        Serial.printf("NTP-Sync erfolgreich (UTC): %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        Serial.printf("NTP-Sync-Marker: %d (1=erfolgreich, 2+=%d fehlgeschlagene Versuche)\n",
                     ntp_sync_marker, ntp_sync_marker > 1 ? ntp_sync_marker - 1 : 0);
        return true;
    } else {
        // Sync fehlgeschlagen: Marker erhöhen (wenn > 0) oder bei 0 bleiben
        if (ntp_sync_marker > 0) {
            ntp_sync_marker++;
            if (ntp_sync_marker > 255) {
                ntp_sync_marker = 0;  // Nach 255 fehlgeschlagenen Versuchen auf 0 zurücksetzen
            }
        }
        Serial.printf("NTP-Sync fehlgeschlagen (Timeout), Marker: %d\n", ntp_sync_marker);
        return false;
    }
}

// ============================================
// Web-Server Handler
// ============================================
String processor(const String& var) {
    // Template-Variablen ersetzen (Default-Werte sind bereits in config_rtc initialisiert)
    if (var == "hostname") {
        return String(config_rtc.hostname);
    } else if (var == "wifi_ssid") {
        // Zeige aktuell verbundenes SSID (falls verbunden)
        if (WiFi.status() == WL_CONNECTED) {
            return WiFi.SSID();
        } else {
            return String("Nicht verbunden");
        }
    } else if (var == "wakeup_minutes") {
        return String(config_rtc.wakeup_minutes);
    } else if (var == "transfer_minutes") {
        return String(config_rtc.transfer_minutes);
    } else if (var == "adc_value") {
        return String(battery_adc_mv);
    } else if (var == "battery_voltage") {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", battery_voltage);
        return String(buf);
    } else if (var == "battery_percent") {
        return String(battery_percent);
    } else if (var == "wakeup_count") {
        return String(wakeupCount);
    } else if (var == "wifi_status") {
        return WiFi.status() == WL_CONNECTED ? "Verbunden" : "Nicht verbunden";
    } else if (var == "wifi_ip") {
        return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "-";
    } else if (var == "wifi_rssi") {
        return WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : "-";
    } else if (var == "ntp_server") {
        return String(config_rtc.ntp_server);
    } else if (var == "system_time") {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            char timeStr[40];
            snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d UTC",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            return String(timeStr);
        } else {
            return String("Nicht synchronisiert");
        }
    } else if (var == "project_name") {
        return String(PROJECT_NAME);
    } else if (var == "project_version") {
        return String(PROJECT_VERSION);
    } else if (var == "build_date") {
        return String(SKETCHCOMPILE);
    } else if (var == "nv_magic_key") {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lu (0x%08lX)", RING_BUFFER_VERSION, RING_BUFFER_VERSION);
        return String(buf);
    } else if (var == "pulse_counter") {
        // Formatierung: 7 Stellen mit führenden Nullen (99999.99 = 9999999)
        char buf[8];  // 7 Stellen + Null-Terminator
        snprintf(buf, sizeof(buf), "%07lu", pulse_counter);
        return String(buf);
    } else if (var == "pulse_counter_left") {
        // Linke 5 Stellen für CSS-Formatierung (Vorkommastellen)
        char buf[6];
        snprintf(buf, sizeof(buf), "%05lu", pulse_counter / 100);
        return String(buf);
    } else if (var == "pulse_counter_right") {
        // Rechte 2 Stellen für CSS-Formatierung (Nachkommastellen)
        char buf[3];  // 2 Stellen + Null-Terminator
        snprintf(buf, sizeof(buf), "%02lu", pulse_counter % 100);
        return String(buf);
    } else if (var == "wifi_info_style") {
        // WiFi-Info Sichtbarkeit: leer wenn verbunden, "display:none;" wenn nicht verbunden
        return WiFi.status() == WL_CONNECTED ? String("") : String("display:none;");
    } else if (var == "adminpass") {
        // Für Config-Seite: Admin-Passwort anzeigen (wird vom Browser als Passwort-Feld behandelt)
        return String(config_rtc.adminpass);
    } else if (var == "adc_voltage_offset") {
        // Für Config-Seite: ADC-Offset als String
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", config_rtc.adc_voltage_offset);
        return String(buf);
    } else if (var == "wifiCredentialsData") {
        // WiFi-Credentials als JSON für JavaScript
        String json = "[";
        for (uint8_t i = 0; i < config_rtc.wifi_count; i++) {
            if (i > 0) json += ",";
            json += "{";
            json += "\"ssid\":\"";
            json += String(config_rtc.wifi_credentials[i].ssid);
            json += "\",\"password\":\"";
            json += String(config_rtc.wifi_credentials[i].password);
            json += "\"";
            json += "}";
        }
        json += "]";
        return json;
    } else if (var == "currentWifiData") {
        // Aktuelle WiFi-Credentials für Vergleich (nur SSID, Passwort nicht ausgeben)
        String json = "[";
        if (WiFi.status() == WL_CONNECTED) {
            String currentSSID = WiFi.SSID();
            // Finde passendes Credential
            for (uint8_t i = 0; i < config_rtc.wifi_count; i++) {
                if (String(config_rtc.wifi_credentials[i].ssid) == currentSSID) {
                    json += "{";
                    json += "\"ssid\":\"";
                    json += currentSSID;
                    json += "\",\"password\":\"";
                    json += String(config_rtc.wifi_credentials[i].password);
                    json += "\"";
                    json += "}";
                    break;
                }
            }
        }
        json += "]";
        return json;
    } else {
        // Unbekannte Variable - leeren String zurückgeben
        return String();
    }
}

void setupWebServer() {
    // WICHTIG: LittleFS muss gemountet sein, bevor Web-Server startet
    // (auch wenn Config bereits aus RTC-RAM geladen wurde)
    if (!mount_littlefs()) {
        Serial.println("FEHLER: LittleFS konnte nicht gemountet werden - Web-Server kann nicht starten!");
        return;
    }
    
    // Root auf index.html umleiten
    server.rewrite("/", "/index.html");
    
    // Index.html Handler: ADC-Werte aktualisieren, dann Template-Processing mit ESPAsyncWebServer Template-Processor
    server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity = millis();
        
        // ADC-Werte bei jedem Seitenaufruf neu messen (für Auto-Refresh)
        battery_adc_mv = read_adc_median_mv();
        battery_voltage = (float)battery_adc_mv / 1000.0f * VOLTAGE_DIVIDER_RATIO + config_rtc.adc_voltage_offset;
        battery_percent = VOLTAGE_TO_PERCENT(battery_voltage);
        
        // Datei mit Template-Processor ausliefern (nutzt %VARIABLE% Syntax)
        // processor() Funktion wird automatisch für jeden %VARIABLE% Platzhalter aufgerufen
        AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/index.html", "text/html", false, processor);
        response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        response->addHeader("Pragma", "no-cache");
        response->addHeader("Expires", "0");
        request->send(response);
    });
    
    // Bootstrap CSS (gzip-komprimiert) - MIT CACHE
    server.on("/bootstrap.min.css", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity = millis();
        
        AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/bootstrap.min.css.gz", "text/css");
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "public, max-age=31536000");  // 1 Jahr Cache
        request->send(response);
    });
    
    // Config-Seite (passwortgeschützt) - mit Template-Processor
    server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
        // Basic Auth prüfen
        if (!request->authenticate("admin", config_rtc.adminpass)) {
            request->requestAuthentication();
            return;
        }
        last_web_activity = millis();
        
        // Datei mit Template-Processor ausliefern
        AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/config.html", "text/html", false, processor);
        response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        request->send(response);
    });
    
    // Config speichern (POST)
    server.on("/config/save", HTTP_POST, [](AsyncWebServerRequest *request){
        last_web_activity = millis();
        
        // POST-Daten lesen
        if (!request->hasParam("data", true)) {
            request->send(400, "text/plain", "Fehler: Parameter 'data' fehlt");
            return;
        }
        
        // Zusätzliche Sicherheit: Aktuelles Passwort muss übergeben werden
        // (verhindert Config-Injection ohne Admin-Passwort)
        String current_password = "";
        if (request->hasParam("current_password", true)) {
            current_password = request->getParam("current_password", true)->value();
        }
        
        // Authentifizierung prüfen:
        // 1. Basic Auth mit aktuellem Passwort ODER
        // 2. current_password Parameter mit aktuellem Passwort
        bool auth_ok = false;
        
        // Prüfe Basic Auth
        if (request->authenticate("admin", config_rtc.adminpass)) {
            auth_ok = true;
        }
        // Prüfe current_password Parameter (falls Basic Auth fehlgeschlagen)
        else if (current_password.length() > 0 && current_password == String(config_rtc.adminpass)) {
            auth_ok = true;
        }
        
        if (!auth_ok) {
            // Wenn weder Basic Auth noch current_password korrekt sind
            request->requestAuthentication();
            return;
        }
        
        String jsonData = request->getParam("data", true)->value();
        
        // JSON parsen
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, jsonData);
        if (error) {
            request->send(400, "text/plain", "Fehler: JSON ungültig");
            return;
        }
        
        // Config in RTC-RAM und config.json speichern
        bool wifi_credentials_changed = false;
        if (save_config(doc, &wifi_credentials_changed)) {
            // Antwort senden, bevor Ressourcen heruntergefahren werden
            request->send(200, "text/plain", "OK - Restart wird durchgeführt...");
            delay(500);  // Kurze Verzögerung, damit die Antwort gesendet wird
            
            // Nach jeder Config-Speicherung: Sauberen Restart durchführen
            // (sicherer, da alle Ressourcen neu initialisiert werden)
            Serial.println("Restart durch Config-Änderung ausgelöst");
            if (wifi_credentials_changed) {
                Serial.println("  → WiFi-Credentials wurden geändert");
            }
            
            // WICHTIG: pulse_counter vor ESP.restart() in Ring-Speicher speichern
            // (RTC-RAM wird bei ESP.restart() zurückgesetzt)
            Serial.println("Speichere pulse_counter in Ring-Speicher vor Reboot...");
            write_pulse_counter_to_ring_buffer();
            
            // Ressourcen sauber beenden
            shutdown_resources();
            
            Serial.println("Starte Reboot...");
            Serial.flush();
            ESP.restart();
            return;  // Wird nie erreicht, aber für Klarheit
        } else {
            request->send(500, "text/plain", "Fehler beim Speichern");
        }
    });
    
    // Reboot-Endpunkt (nur POST mit cmd=reboot, keine Passwort-Abfrage)
    server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *request){
        // Prüfe POST-Parameter: cmd=reboot erforderlich
        if (!request->hasParam("cmd", true) || request->getParam("cmd", true)->value() != "reboot") {
            request->send(400, "text/plain", "Fehler: Parameter 'cmd=reboot' erforderlich");
            return;
        }
        
        last_web_activity = millis();
        request->send(200, "text/plain", "Reboot wird durchgeführt...");
        delay(500);  // Kurze Verzögerung, damit die Antwort gesendet wird
        Serial.println("Reboot durch Web-Interface ausgelöst");
        
        // WICHTIG: pulse_counter vor ESP.restart() in Ring-Speicher speichern
        // (RTC-RAM wird bei ESP.restart() zurückgesetzt)
        Serial.println("Speichere pulse_counter in Ring-Speicher vor Reboot...");
        write_pulse_counter_to_ring_buffer();
        
        // Ressourcen sauber beenden
        shutdown_resources();
        
        Serial.println("Starte Reboot...");
        Serial.flush();
        ESP.restart();
    });
    
    // Reading-Endpunkt: Gibt pulse_counter im Format XXXXX.XX aus (ASCII, für Node-RED etc.)
    server.on("/reading", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity = millis();
        
        // Format: XXXXX.XX (5 Vorkommastellen + Punkt + 2 Nachkommastellen)
        // Beispiel: pulse_counter = 582077 → "05820.77"
        uint32_t vorkommastellen = pulse_counter / 100;
        uint32_t nachkommastellen = pulse_counter % 100;
        
        char reading[9];  // 5 + 1 (Punkt) + 2 + Null-Terminator
        snprintf(reading, sizeof(reading), "%05lu.%02lu", vorkommastellen, nachkommastellen);
        
        request->send(200, "text/plain", reading);
    });
    
    // Version-Endpunkt: Gibt Version, Build-Datum und Projektname aus (ASCII)
    server.on("/version", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity = millis();
        
        // Format: v0.3 (Build 2025-12-30 - 17:00:47) Gas-O-Meter2
        char version[80];  // Ausreichend groß für Format
        snprintf(version, sizeof(version), "v%s (Build %s) %s", 
                 PROJECT_VERSION, SKETCHCOMPILE, PROJECT_NAME);
        
        request->send(200, "text/plain", version);
    });
    
    // Ping-Endpunkt: Gibt "pong" zurück (für Health-Checks)
    server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity = millis();
        request->send(200, "text/plain", "pong");
    });
    
    // Zählerstand manuell setzen
    server.on("/counter/set", HTTP_POST, [](AsyncWebServerRequest *request){
        last_web_activity = millis();
        
        // POST-Parameter prüfen
        if (!request->hasParam("value", true)) {
            request->send(400, "text/plain", "Fehler: Parameter 'value' fehlt");
            return;
        }
        
        String valueStr = request->getParam("value", true)->value();
        uint32_t new_value = (uint32_t)valueStr.toInt();
        
        // Validierung: Maximal 9999999 (99999.99)
        if (new_value > 9999999) {
            request->send(400, "text/plain", "Fehler: Wert zu groß (max. 99999.99)");
            return;
        }
        
        uint32_t old_value = pulse_counter;
        
        Serial.printf("Zählerstand manuell gesetzt: %lu → %lu\n", old_value, new_value);
        
        // Alten Wert in Ring-Speicher schreiben (falls > 0 und > max_pulse)
        // Dies ist wichtig, damit der alte Wert nicht verloren geht
        if (old_value > 0) {
            // Prüfe, ob alter Wert größer als max_pulse im Ring-Speicher ist
            uint32_t max_pulse = 0;
            uint32_t max_index = 0;
            max_pulse = find_max_pulse_and_index_from_nvs(&max_index);
            
            if (old_value > max_pulse) {
                Serial.printf("Alter Wert (%lu) > max_pulse (%lu) → schreibe in Ring-Speicher\n", old_value, max_pulse);
                write_pulse_counter_to_ring_buffer();
            }
        }
        
        // Neuen Wert in RTC-RAM setzen
        pulse_counter = new_value;
        
        // WICHTIG: Stelle sicher, dass NVS initialisiert ist
        init_pulse_nvs_minimal();
        
        // Öffne Ring-Speicher für Schreibzugriff
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open_from_partition(NVS_PARTITION_PULSE, NVS_NAMESPACE_PULSE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) {
            Serial.printf("FEHLER beim Öffnen des NVS-Namespace: %s\n", esp_err_to_name(err));
            request->send(500, "text/plain", "Fehler: NVS-Namespace konnte nicht geöffnet werden");
            return;
        }
        
        // Wenn neuer Wert < alter Wert: Stelle sicher, dass neuer Wert der höchste im Ring-Speicher ist
        // Dazu müssen alle Werte > neuer Wert gelöscht werden
        if (new_value < old_value) {
            Serial.printf("Neuer Wert (%lu) < alter Wert (%lu) → lösche alle Werte > %lu im Ring-Speicher\n", 
                         new_value, old_value, new_value);
            
            uint32_t deleted_count = 0;
            
            // Durchsuche alle möglichen Einträge im Ring-Speicher und lösche Werte > neuer Wert
            for (uint32_t i = 0; i < RING_BUFFER_SIZE; i++) {
                char key[MAX_KEY_LENGTH];
                snprintf(key, sizeof(key), "%s%lu", NVS_KEY_PREFIX, i);
                
                uint32_t pulse_value = 0;
                err = nvs_get_u32(nvs_handle, key, &pulse_value);
                
                if (err == ESP_OK) {
                    // Wert gefunden
                    if (pulse_value > new_value) {
                        // Wert ist größer als neuer Wert → lösche ihn
                        err = nvs_erase_key(nvs_handle, key);
                        if (err == ESP_OK) {
                            deleted_count++;
                            Serial.printf("  Gelöscht: Index %lu = %lu (war > %lu)\n", i, pulse_value, new_value);
                        }
                    }
                }
                // ESP_ERR_NVS_NOT_FOUND ignorieren (Key existiert nicht)
            }
            
            Serial.printf("Ring-Speicher bereinigt: %lu Einträge gelöscht\n", deleted_count);
        }
        
        // Neuen Wert normal in Ring-Speicher schreiben (mit ring_idx)
        // Ring-Index erhöhen
        ring_idx = (ring_idx + 1) % RING_BUFFER_SIZE;
        
        char key[MAX_KEY_LENGTH];
        snprintf(key, sizeof(key), "%s%lu", NVS_KEY_PREFIX, ring_idx);
        
        err = nvs_set_u32(nvs_handle, key, new_value);
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
            if (err == ESP_OK) {
                Serial.printf("Neuer Zählerstand (%lu) in Ring-Speicher geschrieben (Index: %lu)\n", new_value, ring_idx);
            } else {
                Serial.printf("FEHLER beim Committen: %s\n", esp_err_to_name(err));
            }
        } else {
            Serial.printf("FEHLER beim Schreiben: %s\n", esp_err_to_name(err));
        }
        
        nvs_close(nvs_handle);
        
        // Erfolgreiche Antwort
        char response[100];
        snprintf(response, sizeof(response), "Zählerstand gesetzt: %05lu.%02lu", new_value / 100, new_value % 100);
        request->send(200, "text/plain", response);
    });
    
    // WiFi-Scan-Endpunkt: Gibt verfügbare WLAN-Netzwerke zurück (passwortgeschützt)
    server.on("/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *request){
        // Basic Auth prüfen
        if (!request->authenticate("admin", config_rtc.adminpass)) {
            request->requestAuthentication();
            return;
        }
        last_web_activity = millis();
        
        // WiFi-Modus sicherstellen (Station-Modus für Scan erforderlich)
        WiFiMode_t currentMode = WiFi.getMode();
        if (currentMode == WIFI_OFF || currentMode == WIFI_AP) {
            // WiFi in Station-Modus versetzen (falls nicht bereits)
            WiFi.mode(WIFI_STA);
            delay(100);
        }
        
        // WiFi-Scan durchführen
        Serial.println("WiFi-Scan wird durchgeführt...");
        int n = WiFi.scanNetworks();
        
        if (n < 0) {
            // Fehler beim Scan
            request->send(500, "application/json", "{\"error\":\"WiFi-Scan fehlgeschlagen\"}");
            return;
        }
        
        if (n == 0) {
            request->send(200, "application/json", "[]");
            return;
        }
        
        // Netzwerke nach RSSI sortieren (höchster zuerst)
        struct NetworkInfo {
            String ssid;
            int rssi;
            bool encrypted;
        };
        
        NetworkInfo networks[n];
        for (int i = 0; i < n; i++) {
            networks[i].ssid = WiFi.SSID(i);
            networks[i].rssi = WiFi.RSSI(i);
            networks[i].encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        }
        
        // Einfache Sortierung nach RSSI (Bubble Sort - ausreichend für max. 10 Netzwerke)
        for (int i = 0; i < n - 1; i++) {
            for (int j = 0; j < n - i - 1; j++) {
                if (networks[j].rssi < networks[j + 1].rssi) {
                    NetworkInfo temp = networks[j];
                    networks[j] = networks[j + 1];
                    networks[j + 1] = temp;
                }
            }
        }
        
        // JSON erstellen (max. 10 stärkste Netzwerke)
        JsonDocument doc;
        JsonArray networksArray = doc.to<JsonArray>();
        int maxNetworks = (n > 10) ? 10 : n;
        
        for (int i = 0; i < maxNetworks; i++) {
            JsonObject network = networksArray.add<JsonObject>();
            network["ssid"] = networks[i].ssid;
            network["rssi"] = networks[i].rssi;
            network["encrypted"] = networks[i].encrypted;
        }
        
        String jsonResponse;
        serializeJson(doc, jsonResponse);
        request->send(200, "application/json", jsonResponse);
    });
    
    // Alle anderen Dateien aus Filesystem (außer config.json)
    server.serveStatic("/", LittleFS, "/")
        .setFilter([](AsyncWebServerRequest *request){
            last_web_activity = millis();
            
            String path = request->url();
            
            // Blockiere config.json
            if (path.equals("/config.json")) {
                request->send(403, "text/plain", "Forbidden: config.json is protected");
                return false;
            }
            
            return true;
        });
    
    server.begin();
    server_started = true;  // Flag setzen: Web-Server läuft
    last_web_activity = millis();  // Initialisierung als Startpunkt
}

// ============================================
// Setup
// ============================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    // Antennenumschaltung initialisieren (interne Antenne als Standard)
    INIT_ANTENNA_SWITCH(ANTENNA_INTERNAL);
    
    // Taster A (BUTTON_A_GPIO) für Wake-up konfigurieren (INPUT_PULLUP, active-low)
    pinMode(BUTTON_A_GPIO, BUTTON_A_GPIO_MODE);
    
    // Power-On vs. Wake-up erkennen
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    isPowerOn = (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED);
    
    // Wake-up Count nur bei Deep-Sleep-Wake-up erhöhen (nicht bei ESP.restart())
    if (!isPowerOn) {
        ++wakeupCount;
    }
    
    // Wake-up-Informationen ausgeben (kombiniert)
    Serial.println("\n=== Gas-O-Meter ===");
    Serial.printf("Wake-up Count: %d\n", wakeupCount);
    
    switch (wakeup_reason) {
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            Serial.println("=== EVENT: Power-On ===");
            break;
        case ESP_SLEEP_WAKEUP_GPIO:
            Serial.println("=== EVENT: Wake-up durch GPIO (Taster A) ===");
            break;
        case ESP_SLEEP_WAKEUP_TIMER:
            Serial.println("=== EVENT: Wake-up durch Timer (Cron-Intervall) ===");
            break;
        case ESP_SLEEP_WAKEUP_EXT0:
        case ESP_SLEEP_WAKEUP_EXT1:
            Serial.println("=== EVENT: Wake-up durch GPIO (EXT) ===");
            break;
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
            Serial.println("=== EVENT: Wake-up durch Touchpad ===");
            break;
        case ESP_SLEEP_WAKEUP_ULP:
            Serial.println("=== EVENT: Wake-up durch ULP ===");
            break;
        default:
            Serial.printf("=== EVENT: Wake-up durch Unbekannt (0x%x) ===\n", wakeup_reason);
            break;
    }
    
    // Interne LED initialisieren und sofort einschalten (HP-Core läuft)
    // WICHTIG: LED wird hier eingeschaltet, um zu zeigen, dass HP-Core aktiv ist
    pinMode(LED_BUILTIN_GPIO, OUTPUT);
    digitalWrite(LED_BUILTIN_GPIO, LED_ON);  // LED EIN (HP-Core aktiv)
    Serial.printf("LED initialisiert (GPIO%d)\n", LED_BUILTIN_GPIO);
    
    // ADC initialisieren (analogReadMilliVolts - automatisch kalibriert)
    // WICHTIG: Spannungsprüfung VOR NVS-Initialisierung, um Akku zu schützen!
    esp_err_t adc_ret = init_adc();
    if (adc_ret != ESP_OK) {
        Serial.printf("ADC Fehler: %s\n", esp_err_to_name(adc_ret));
    } else {
        // Akku-Messung (analogReadMilliVolts gibt direkt Millivolt zurück)
        battery_adc_mv = read_adc_median_mv();
        battery_voltage = (float)battery_adc_mv / 1000.0f * VOLTAGE_DIVIDER_RATIO + config_rtc.adc_voltage_offset;
        battery_percent = VOLTAGE_TO_PERCENT(battery_voltage);
        
        Serial.printf("ADC: %d mV, Spannung: %.2f V, Prozent: %d%%\n", 
                     battery_adc_mv, battery_voltage, battery_percent);
        
        // Stromversorgungs-Prüfung ZUERST (vor Akku-Schutz)
        // WICHTIG: Bei USB-Stromversorgung kann Timer aktiv bleiben (keine Akku-Probleme)
        bool is_usb_power = (battery_voltage < USB_DETECTION_THRESHOLD);
        bool enable_timer_wakeup = true;  // Standard: Timer aktiviert
        
        if (is_usb_power) {
            // USB-Stromversorgung: Timer kann aktiv bleiben (keine Akku-Probleme)
            Serial.println("USB-Stromversorgung erkannt - Betrieb fortgesetzt");
            Serial.printf("Spannung: %.2f V (USB-Schwelle: %.2f V)\n", 
                         battery_voltage, USB_DETECTION_THRESHOLD);
            // enable_timer_wakeup bleibt true
        } else {
            // Akku-Betrieb: Timer-Wake-up deaktivieren bei kritischer Spannung
            // WICHTIG: Bei Spannung <= BATTERY_VOLTAGE_PROTECTION macht automatisches Wake-up nur mehr Schaden
            // Nur noch manueller Wake-up über Taster möglich
            if (battery_voltage <= BATTERY_VOLTAGE_PROTECTION) {
                enable_timer_wakeup = false;
                Serial.println("WARNUNG: Akku-Spannung kritisch (<= BATTERY_VOLTAGE_PROTECTION)");
                Serial.println("Timer-Wake-up wird DEAKTIVIERT - nur manueller Wake-up über Taster A möglich");
                Serial.printf("Spannung: %.2f V (Schutz-Schwelle: %.2f V)\n", 
                             battery_voltage, BATTERY_VOLTAGE_PROTECTION);
            }
            
            // Akku-Schutz: Deep-Sleep bei zu niedriger Spannung
            if (battery_voltage < BATTERY_VOLTAGE_20) {
                // Spannung >= 2V aber < BATTERY_VOLTAGE_20: Akku zu niedrig → Deep-Sleep zum Schutz
                Serial.println("Akku zu niedrig - Deep-Sleep zum Akku-Schutz");
                Serial.printf("Spannung: %.2f V (Minimum: %.2f V)\n", 
                             battery_voltage, BATTERY_VOLTAGE_20);
                
                // Vor Deep-Sleep: pulse_counter prüfen und ggf. in Ring-Speicher schreiben
                // (bei Akku-Low kann RTC-RAM verloren gehen)
                Serial.println("Speichere pulse_counter in Ring-Speicher vor Deep-Sleep (Akku-Low)...");
                write_pulse_counter_to_ring_buffer();
                
                // Deep-Sleep mit GPIO-Wake-up (Taster A) - spart Energie und schützt Akku
                // Timer-Wake-up: Bei USB immer aktiv, sonst nur wenn Spannung > BATTERY_VOLTAGE_PROTECTION
                enter_deep_sleep_with_gpio_and_timer_wakeup(enable_timer_wakeup);
                // Ab hier wird Code nicht mehr ausgeführt (Deep-Sleep)
                return;
            }
        }
        
        // NVS initialisieren (bei Power-On und Deep-Sleep-Wake-up)
        // WICHTIG: nvs_flash_init() ist idempotent - wenn bereits initialisiert, passiert nichts (keine Wear!)
        init_nvs_partitions(isPowerOn);
        
        // NVS-Ring-Speicher: Versionsnummer prüfen und ggf. initialisieren
        // Dies geschieht NUR beim ersten Boot nach Code-Upload oder partition.csv-Änderung
        if (isPowerOn) {
            Serial.println("Prüfe NVS-Ring-Speicher-Version...");
            Serial.printf("Erwartete Version (Build-Timestamp): %lu\n", RING_BUFFER_VERSION);
            check_and_init_pulse_ring_nvs();
        }
        
        // LP-Core Watchdog Task starten (asynchron)
        // Der Task prüft automatisch lp_core_running und startet LP-Core bei Bedarf
        // WICHTIG: Task wird sowohl bei Power-On als auch bei Deep-Sleep-Wake-up gestartet
        xTaskCreate(
            lp_core_watchdog_task,      // Task-Funktion
            "LP_Core_Watchdog",          // Task-Name
            4096,                        // Stack-Größe (Bytes)
            NULL,                        // Parameter
            1,                           // Priorität (niedrig, da nicht kritisch)
            NULL                         // Task-Handle (nicht benötigt)
        );
        Serial.println("LP-Core Watchdog Task gestartet");
        
        // ring_idx und pulse_counter initialisieren (kombiniert)
        // ring_idx: Ring-Buffer-Index (aus RTC-RAM oder Ring-Speicher)
        // pulse_counter: Puls-Zähler (aus RTC-RAM oder Ring-Speicher)
        init_ring_buffer_and_pulse_counter(isPowerOn);
        
        // 1. Batteriespannung-Test und ggf. in Ring-Speicher schreiben
        // < 30% ODER USB: Schreibe in Ring-Speicher (RTC-RAM könnte verloren gehen)
        // >= 30%: Kein Schreiben (RTC-RAM bleibt erhalten)
        if (battery_voltage < BATTERY_VOLTAGE_30 || battery_voltage < USB_DETECTION_THRESHOLD) {
            Serial.println("Speichere pulse_counter in Ring-Speicher (< 30% oder USB)...");
            write_pulse_counter_to_ring_buffer();
        }
        
        // 2. Config laden (idempotent: prüft intern, ob bereits geladen)
        // WICHTIG: Muss vor allen Aktionen geladen sein, da beide Stränge (Timer/Power-On/GPIO) Config benötigen
        bool config_available = load_config();
        
        // 3. Abhängig vom Wake-up-Grund: Unterschiedliche Aktionen
        switch (wakeup_reason) {
            case ESP_SLEEP_WAKEUP_TIMER:
                // Timer-Wake-up: Prüfen, ob Datenübertragung fällig ist
                // Übertragung nur alle X Timer-Wake-ups (basierend auf transfer_minutes)
                struct tm timeinfo;
                if (getLocalTime(&timeinfo) && (timeinfo.tm_min % config_rtc.transfer_minutes == 0) && config_available) {
                    Serial.printf("=== Timer-Wake-up: Datenübertragung (Minute %d, Intervall: %d Min) ===\n",
                                 timeinfo.tm_min, config_rtc.transfer_minutes);
                    Serial.println("Hier sollten jetzt die Daten übertragen werden....................");
                    should_enter_deep_sleep = true;
                    deep_sleep_reason = "Timer-Wake-up: Datenübertragung abgeschlossen";
                } else {
                    uint8_t current_min = getLocalTime(&timeinfo) ? timeinfo.tm_min : 0;
                    Serial.printf("=== Timer-Wake-up: Keine Übertragung (Minute %d, Intervall: %d Min) ===\n",
                                 current_min, config_rtc.transfer_minutes);
                    should_enter_deep_sleep = true;
                    deep_sleep_reason = (!config_available) ? "Timer-Wake-up: Config-Fehler" : "Timer-Wake-up: Keine Übertragung fällig";
                }
                break;
                
            case ESP_SLEEP_WAKEUP_GPIO:
            case ESP_SLEEP_WAKEUP_UNDEFINED:
            default:
                // Power-On oder GPIO-Wake-up: WiFi und Web-Server starten
                if (config_available) {
                    Serial.println("Config erfolgreich geladen");
                    
                    // WiFi verbinden
                    if (connect_wifi()) {
                        // mDNS starten (für .local Domain)
                        // HINWEIS: ESP32C6 ESPmDNS läuft automatisch asynchron im Hintergrund
                        if (MDNS.begin(config_rtc.hostname)) {
                            Serial.printf("mDNS: http://%s.local\n", config_rtc.hostname);
                        }
                        
                        // NTP-Zeitsynchronisation
                        sync_ntp_time();
                        
                        // Web-Server starten
                        setupWebServer();
                        Serial.printf("Web-Server: http://%s\n", WiFi.localIP().toString().c_str());
                    } else {
                        Serial.println("WiFi-Verbindung fehlgeschlagen");
                    }
                } else {
                    Serial.println("Config-Laden fehlgeschlagen → WiFi/Web-Server nicht gestartet");
                }
                break;
        }
    }  // Ende des else-Blocks (ADC erfolgreich)
}

void loop() {
    // AsyncWebServer läuft im Hintergrund, kein handleClient() nötig
    // mDNS läuft automatisch asynchron im Hintergrund (nach MDNS.begin())
    
    // Prüfe Web-Server-Inaktivität: Wenn WIFI_WAIT_FOR_SLEEP Minuten keine Aktivität → Deep-Sleep anfordern
    if (server_started) {  // Web-Server läuft
        unsigned long inactivity_ms = millis() - last_web_activity;
        unsigned long sleep_threshold_ms = WIFI_WAIT_FOR_SLEEP * 60 * 1000UL;  // Minuten in Millisekunden
        
        if (inactivity_ms >= sleep_threshold_ms && !should_enter_deep_sleep) {
            Serial.printf("Keine Web-Server-Aktivität seit %lu Minuten → Deep-Sleep anfordern\n", WIFI_WAIT_FOR_SLEEP);
            should_enter_deep_sleep = true;
            deep_sleep_reason = "Web-Server-Inaktivität";
        }
    }
    
    // Prüfe, ob Deep-Sleep gewünscht ist (Timer-Wake-up, Web-Timeout, etc.)
    if (should_enter_deep_sleep) {
        Serial.println(deep_sleep_reason);
        
        // Ring-Speicher-Prüfung (einmalig, zentralisiert)
        // < 30% ODER USB: Schreibe in Ring-Speicher (RTC-RAM könnte verloren gehen)
        // >= 30%: Kein Schreiben (RTC-RAM bleibt erhalten)
        if (battery_voltage < BATTERY_VOLTAGE_30 || battery_voltage < USB_DETECTION_THRESHOLD) {
            Serial.println("Speichere pulse_counter in Ring-Speicher vor Deep-Sleep (< 30% oder USB)...");
            write_pulse_counter_to_ring_buffer();
        } else {
            Serial.println("Akku-Spannung OK (>= 30%) → pulse_counter bleibt im RTC-RAM (kein Schreiben nötig)");
        }
        
        // Timer-Wake-up: Bei USB-Stromversorgung immer aktivieren
        // Bei Akku-Betrieb: Nur aktivieren, wenn Spannung > BATTERY_VOLTAGE_PROTECTION
        bool is_usb_power = (battery_voltage < USB_DETECTION_THRESHOLD);
        bool enable_timer = is_usb_power || (battery_voltage > BATTERY_VOLTAGE_PROTECTION);
        enter_deep_sleep_with_gpio_and_timer_wakeup(enable_timer);
        
        // Wenn wir hier ankommen, wurde Deep-Sleep nicht gestartet (z.B. keine Wake-up-Quelle)
        // Flag zurücksetzen, um Endlosschleife zu vermeiden
        should_enter_deep_sleep = false;
        Serial.println("Deep-Sleep konnte nicht gestartet werden - System bleibt aktiv");
        return;  // Loop beenden, wird beim nächsten Durchlauf erneut geprüft
    }
    
    delay(100);
}

