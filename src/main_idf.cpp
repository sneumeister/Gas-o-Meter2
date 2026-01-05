// ESP-IDF Headers
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "esp_littlefs.h"
#include "lwip/apps/sntp.h"
#include "esp_vfs.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
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

// ArduinoJson (ESP-IDF-kompatibel)
#include <ArduinoJson.h>

// Template-Placeholder (aus CMakeLists.txt)
#ifndef TEMPLATE_PLACEHOLDER
#define TEMPLATE_PLACEHOLDER '`'   // Backtick (`) für Template-Platzhalter
#endif

// Logging-Tag
static const char *TAG = "gas-o-meter";

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
httpd_handle_t server = NULL;  // ESP-IDF HTTP Server Handle
bool littlefs_mounted = false;
bool server_started = false;  // Flag: Web-Server gestartet?

// WiFi-Status-Variablen
bool wifi_connected = false;
bool wifi_initialized = false;  // Flag: WiFi initialisiert?
wifi_ap_record_t ap_info;  // Für WiFi-Scan-Ergebnisse
esp_netif_ip_info_t wifi_ip_info;  // WiFi-IP-Informationen

// Akku-Messwerte
uint32_t battery_adc_mv = 0;  // ADC-Wert in Millivolt
float battery_voltage = 0.0f;
uint8_t battery_percent = 0;

// ADC-Handles (ESP-IDF)
adc_oneshot_unit_handle_t adc1_handle = NULL;
adc_cali_handle_t adc1_cali_handle = NULL;

// Web-Server Inaktivitäts-Timer
volatile uint64_t last_web_activity_us = 0;  // Zeitpunkt der letzten Web-Server-Aktivität (Mikrosekunden)

// Deep-Sleep-Steuerung (zentralisiert in web_timeout_task)
bool should_enter_deep_sleep = false;
const char* deep_sleep_reason = NULL;

// Reboot-Steuerung (wie Arduino: Variable setzen, Reboot im Task)
bool reboot_requested = false;
const char* reboot_reason = NULL;

// Funktionsdeklarationen
void web_timeout_task(void *parameter);

// ============================================
// ADC Initialisierung (ESP-IDF)
// ============================================
esp_err_t init_adc() {
    // ADC Unit initialisieren
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_config1, &adc1_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC Unit Initialisierung fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // ADC Channel konfigurieren
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_oneshot_config_channel(adc1_handle, BATTERY_ADC_CHANNEL, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC Channel Konfiguration fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // ADC Kalibrierung (Curve Fitting)
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_config, &adc1_cali_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC Kalibrierung fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "ADC initialisiert (Channel %d, Unit 1)", BATTERY_ADC_CHANNEL);
    return ESP_OK;
}

// ============================================
// ADC-Messung mit Median-Filter (in Millivolt)
// ============================================
uint32_t read_adc_median_mv() {
    uint32_t adc_values[ADC_SAMPLE_COUNT];
    
    // Insertion Sort beim Einlesen (effizienter für kleine Arrays)
    for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
        // ADC-Rohwert lesen
        int adc_raw;
        esp_err_t ret = adc_oneshot_read(adc1_handle, BATTERY_ADC_CHANNEL, &adc_raw);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC-Lesefehler: %s", esp_err_to_name(ret));
            continue;
        }
        
        // In Millivolt konvertieren (kalibriert)
        int voltage_mv;
        ret = adc_cali_raw_to_voltage(adc1_cali_handle, adc_raw, &voltage_mv);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC-Kalibrierungsfehler: %s", esp_err_to_name(ret));
            continue;
        }
        
        uint32_t mv = (uint32_t)voltage_mv;
        
        // Einfügen und gleichzeitig sortieren (Insertion Sort)
        int j = i;
        while (j > 0 && adc_values[j - 1] > mv) {
            adc_values[j] = adc_values[j - 1];
            j--;
        }
        adc_values[j] = mv;
        
        vTaskDelay(pdMS_TO_TICKS(10));
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
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "LittleFS Mount fehlgeschlagen: Dateisystem konnte nicht formatiert werden");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "LittleFS Mount fehlgeschlagen: Partition nicht gefunden");
        } else {
            ESP_LOGE(TAG, "LittleFS Mount fehlgeschlagen: %s", esp_err_to_name(ret));
        }
        return false;
    }
    
    littlefs_mounted = true;
    ESP_LOGI(TAG, "LittleFS erfolgreich gemountet");
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
        ESP_LOGE(TAG, "NVS-Namespace '%s' in Partition '%s' konnte nicht geöffnet werden: %s", 
                 NVS_NAMESPACE_PULSE, NVS_PARTITION_PULSE, esp_err_to_name(err));
        if (out_max_index != nullptr) {
            *out_max_index = 0;
        }
        return 0;  // Keine Daten vorhanden
    }
    
    ESP_LOGI(TAG, "Durchsuche NVS-Ring-Speicher '%s' nach höchstem Puls-Zähler-Wert...", 
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
            ESP_LOGI(TAG, "NVS-Ring-Speicher: %lu Einträge gefunden, höchster Wert: %lu (Index: %lu)", 
                     found_count, max_pulse, max_index);
        } else {
            // Nur Initialisierungsreste gefunden (alle Werte = 0)
            ESP_LOGI(TAG, "NVS-Ring-Speicher: %lu Initialisierungsreste gefunden (alle 0) - keine echten Daten", 
                     found_count);
            max_pulse = 0;  // Als "keine Daten" behandeln
        }
    } else {
        ESP_LOGI(TAG, "NVS-Ring-Speicher: Keine Einträge gefunden");
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
        ESP_LOGI(TAG, "Pulse-NVS-Partition '%s' benötigt Initialisierung...", NVS_PARTITION_PULSE);
        // Bei Wake-up: Versuche erneut zu initialisieren (ohne Löschung)
        err = nvs_flash_init_partition(NVS_PARTITION_PULSE);
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Pulse-NVS-Initialisierung fehlgeschlagen: %s", esp_err_to_name(err));
        return false;
    }
    
    ESP_LOGI(TAG, "Pulse-NVS-Partition '%s' erfolgreich initialisiert", NVS_PARTITION_PULSE);
    return true;
}

// ============================================
// RTC pulse_counter: In Ring-Speicher schreiben (bei ESP.restart(), Akku-Low, USB)
// ============================================
bool write_pulse_counter_to_ring_buffer() {
    // WICHTIG: Stelle sicher, dass Pulse-NVS initialisiert ist
    if (!init_pulse_nvs_minimal()) {
        ESP_LOGE(TAG, "FEHLER: Pulse-NVS konnte nicht initialisiert werden → kein Schreiben möglich");
        return false;
    }
    
    // Prüfung: Nur schreiben, wenn pulse_counter > 0
    if (pulse_counter == 0) {
        ESP_LOGI(TAG, "pulse_counter ist 0 → Keine Ring-Speicher-Schreibung nötig");
        return true;  // Kein Fehler, einfach nichts zu speichern
    }
    
    // Prüfung: Nur schreiben, wenn pulse_counter > max_pulse aus Ring-Speicher
    uint32_t max_pulse = find_max_pulse_from_nvs();
    if (pulse_counter <= max_pulse) {
        ESP_LOGI(TAG, "pulse_counter nicht gespeichert: RTC=%lu <= Ring-Speicher-Max=%lu", 
                 pulse_counter, max_pulse);
        return true;  // Kein Fehler, einfach nichts zu speichern
    }
    
    // Ring-Speicher öffnen
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open_from_partition(NVS_PARTITION_PULSE, NVS_NAMESPACE_PULSE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fehler beim Öffnen von Ring-Speicher für pulse_counter: %s", esp_err_to_name(err));
        return false;
    }
    
    // ring_idx aus RTC-RAM verwenden (nicht aus NVS lesen!)
    // WICHTIG: ring_idx wird im RTC-RAM gehalten, um Wear-Leveling-Hotspot zu vermeiden
    if (ring_idx >= RING_BUFFER_SIZE) {
        // ring_idx ist ungültig (z.B. nach Power-On, aber noch nicht initialisiert)
        ESP_LOGW(TAG, "WARNUNG: ring_idx ist ungültig! Sollte beim Boot initialisiert werden.");
        nvs_close(nvs_handle);
        return false;
    }
    
    // pulse_counter an Position ring_idx schreiben
    char key[MAX_KEY_LENGTH];
    snprintf(key, sizeof(key), "%s%lu", NVS_KEY_PREFIX, ring_idx);
    err = nvs_set_u32(nvs_handle, key, pulse_counter);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fehler beim Schreiben von pulse_counter in Ring-Speicher: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    // Ring-Index inkrementieren (modulo RING_BUFFER_SIZE) - NUR im RTC-RAM!
    // NICHT mehr in NVS speichern (vermeidet Wear-Leveling-Hotspot)
    ring_idx = (ring_idx + 1) % RING_BUFFER_SIZE;
    
    // Änderungen committen
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fehler beim Commit von Ring-Speicher: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "pulse_counter in Ring-Speicher geschrieben: %lu (Position: %lu, nächster Index: %lu)", 
             pulse_counter, (ring_idx == 0 ? RING_BUFFER_SIZE - 1 : ring_idx - 1), ring_idx);
    return true;
}


// FreeRTOS Task für LP-Core Watchdog
void lp_core_watchdog_task(void *parameter) {
    uint32_t last_lp_core_value = 0;
    uint8_t retry_count = 0;
    const uint8_t MAX_RETRIES = 3;
    
    ESP_LOGI(TAG, "LP-Core Watchdog Task gestartet");
    
    // Initialisiere last_lp_core_value mit aktuellem Wert
    last_lp_core_value = lp_core_running;
    if (last_lp_core_value > 0) {
        ESP_LOGI(TAG, "LP-Core läuft bereits (Zähler: %lu)", last_lp_core_value);
    }
    
    // Kombinierte Start- und Watchdog-Schleife
    // Wenn lp_core_running == 0 ODER Counter erhöht sich nicht → versuche LP-Core zu starten
    // Wenn nach MAX_RETRIES immer noch nicht erfolgreich → Task beenden
    while (1) {
        // Prüfe ob LP-Core läuft (lp_core_running == 0 bedeutet: nicht gestartet oder gestoppt)
    if (lp_core_running == 0) {
            // LP-Core läuft nicht → versuche zu starten
            retry_count++;
            ESP_LOGW(TAG, "LP-Core läuft nicht (lp_core_running == 0) → Starte LP-Core... (Versuch %d/%d)", 
                     retry_count, MAX_RETRIES);
            
            if (retry_count >= MAX_RETRIES) {
                ESP_LOGE(TAG, "FEHLER: LP-Core konnte nach %d Versuchen nicht gestartet werden. Watch-Dog-Task beendet!", MAX_RETRIES);
            vTaskDelete(NULL);
            return;
        }
            
            // Versuche LP-Core zu starten
            if (start_lp_core()) {
                // start_lp_core() gab true zurück - warte auf Watchdog-Timeout und prüfe dann
        last_lp_core_value = lp_core_running;
                ESP_LOGI(TAG, "LP-Core Start aufgerufen (Zähler: %lu) - warte auf Watchdog-Timeout für Prüfung...", last_lp_core_value);
            } else {
                // start_lp_core() gab false zurück
                ESP_LOGE(TAG, "FEHLER: LP-Core Start fehlgeschlagen (start_lp_core() gab false zurück)!");
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
            ESP_LOGW(TAG, "WARNUNG: LP-Core Watchdog-Timeout! (Zähler: %lu, erwartet: > %lu)", 
                     current_lp_core_value, last_lp_core_value);
                ESP_LOGI(TAG, "Setze lp_core_running auf 0 und versuche LP-Core neu zu starten...");
            
                // Setze lp_core_running auf 0, damit wir in die Start-Schleife kommen
                lp_core_running = 0;
                retry_count = 0;  // Reset Retry-Counter für Neustart-Versuche
                continue;  // Gehe zurück in Start-Schleife
            } else {
                // Zähler hat sich erhöht → LP-Core läuft korrekt
            ESP_LOGI(TAG, "LP-Core Watchdog OK (Zähler: %lu → %lu)", 
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
        ESP_LOGE(TAG, "NVS-Namespace '%s' in Partition '%s' konnte nicht geöffnet werden: %s", 
                 NVS_NAMESPACE_PULSE, NVS_PARTITION_PULSE, esp_err_to_name(err));
        return false;
    }
    
    ESP_LOGI(TAG, "Initialisiere NVS-Ring-Speicher (Metadaten)...");
    
    // Alte Initialisierungsreste löschen (falls vorhanden)
    // Wir löschen alle p_* Keys, die möglicherweise von einem vorherigen fehlgeschlagenen Versuch stammen
    // Dies ist sicher, da wir Lazy Initialization verwenden und nur Metadaten speichern
    ESP_LOGI(TAG, "Lösche eventuelle Initialisierungsreste...");
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
        ESP_LOGE(TAG, "FEHLER beim Setzen der Versionsnummer: %s", esp_err_to_name(err));
        success = false;
    }
    
    // Änderungen committen
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FEHLER beim Committen der NVS-Änderungen: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    nvs_close(nvs_handle);
    
    if (success) {
        ESP_LOGI(TAG, "NVS-Ring-Speicher erfolgreich initialisiert (Version %lu, Timestamp: %lu)", 
                 version, version);
    } else {
        ESP_LOGW(TAG, "WARNUNG: NVS-Ring-Speicher-Initialisierung mit Fehlern abgeschlossen!");
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
        ESP_LOGI(TAG, "NVS-Namespace '%s' in Partition '%s' existiert nicht → Initialisierung erforderlich", 
                 NVS_NAMESPACE_PULSE, NVS_PARTITION_PULSE);
        return init_pulse_ring_nvs();
    }
    
    // Versionsnummer lesen
    uint32_t stored_version = 0;
    err = nvs_get_u32(nvs_handle, NVS_KEY_VERSION, &stored_version);
    nvs_close(nvs_handle);
    
    if (err != ESP_OK) {
        // Versionsnummer existiert nicht → Initialisierung erforderlich
        ESP_LOGI(TAG, "Versionsnummer nicht gefunden → Initialisierung erforderlich");
        return init_pulse_ring_nvs();
    }
    
    if (stored_version != RING_BUFFER_VERSION) {
        // Versionsnummer stimmt nicht überein → Initialisierung erforderlich
        ESP_LOGI(TAG, "Versionsnummer stimmt nicht überein (gespeichert: %lu, erwartet: %lu) → Initialisierung erforderlich",
                 stored_version, RING_BUFFER_VERSION);
        return init_pulse_ring_nvs();
    }
    
    // Versionsnummer stimmt → keine Initialisierung nötig
    ESP_LOGI(TAG, "NVS-Ring-Speicher Version %lu ist gültig → keine Initialisierung nötig", stored_version);
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
            ESP_LOGI(TAG, "%s muss neu initialisiert werden...", partition_name);
            esp_err_t erase_err = is_standard_nvs ? 
                nvs_flash_erase() : 
                nvs_flash_erase_partition(partition_name);
            if (erase_err == ESP_OK) {
                err = is_standard_nvs ? 
                    nvs_flash_init() : 
                    nvs_flash_init_partition(partition_name);
            } else {
                ESP_LOGE(TAG, "%s-Löschung fehlgeschlagen: %s", partition_name, esp_err_to_name(erase_err));
            }
        } else {
            // Bei Deep-Sleep-Wake-up: Versuche erneut ohne Löschung (NVS sollte noch vorhanden sein)
            ESP_LOGW(TAG, "WARNUNG: %s benötigt Neuinitialisierung!", partition_name);
            ESP_LOGI(TAG, "Versuche erneut zu initialisieren (ohne Löschung)...");
            err = is_standard_nvs ? 
                nvs_flash_init() : 
                nvs_flash_init_partition(partition_name);
        }
    }
    
    // Ergebnis prüfen
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s-Initialisierung fehlgeschlagen: %s", partition_name, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "%s erfolgreich initialisiert", partition_name);
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
            ESP_LOGI(TAG, "ring_idx aus Ring-Speicher: %lu (höchster Index: %lu)", ring_idx, max_index);
        } else {
            ring_idx = 0;  // Erster Slot (keine Daten vorhanden)
            ESP_LOGI(TAG, "ring_idx auf 0 gesetzt (keine Daten im Ring-Speicher)");
        }
    } else {
        // Bei Deep-Sleep-Wake-up: ring_idx aus RTC-RAM übernehmen
        ESP_LOGI(TAG, "ring_idx aus RTC-RAM: %lu", ring_idx);
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
            ESP_LOGI(TAG, "pulse_counter aus Ring-Speicher: %lu", pulse_counter);
        } else {
            pulse_counter = 0;
            ESP_LOGI(TAG, "pulse_counter auf 0 initialisiert (keine Ring-Speicher-Daten)");
        }
    } else {
        // RTC-RAM noch vorhanden (bei Deep-Sleep-Wake-up)
        if (!is_power_on) {
            ESP_LOGI(TAG, "pulse_counter aus RTC-RAM: %lu (RTC-RAM behält Daten bei Deep-Sleep-Wake-up)", pulse_counter);
        } else {
            ESP_LOGI(TAG, "pulse_counter aus RTC-RAM: %lu", pulse_counter);
        }
    }
}

// ============================================
// Ressourcen sauber beenden (Web-Server, WiFi, LittleFS)
// ============================================
void shutdown_resources() {
    ESP_LOGI(TAG, "Schließe Ressourcen...");
    
    // 1. mDNS stoppen
    mdns_free();
    ESP_LOGI(TAG, "mDNS gestoppt");
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 2. Web-Server stoppen
    if (server_started && server != NULL) {
        ESP_LOGI(TAG, "Stoppe Web-Server...");
        esp_err_t ret = httpd_stop(server);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "httpd_stop() gab Fehler zurück: %s", esp_err_to_name(ret));
        }
        server = NULL;
        server_started = false;  // Flag zurücksetzen
        ESP_LOGI(TAG, "Web-Server gestoppt");
        vTaskDelay(pdMS_TO_TICKS(100));  // Kurze Verzögerung für sauberes Schließen
    }
    
    // 3. WiFi trennen und stoppen (STA und/oder AP)
    if (wifi_initialized) {
        ESP_LOGI(TAG, "Trenne WiFi...");
        // WiFi trennen (falls im STA-Modus verbunden)
        if (wifi_connected) {
            esp_err_t ret = esp_wifi_disconnect();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_disconnect() gab Fehler zurück: %s", esp_err_to_name(ret));
            }
            wifi_connected = false;
        }
        
        // WiFi stoppen (funktioniert für STA und AP)
        esp_err_t ret = esp_wifi_stop();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_stop() gab Fehler zurück: %s", esp_err_to_name(ret));
        }
        ESP_LOGI(TAG, "WiFi getrennt und gestoppt");
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // WiFi deinitialisieren (optional, spart etwas RAM)
        // esp_wifi_deinit();  // Auskommentiert, da WiFi beim nächsten Wake-up wieder initialisiert wird
    }
    
    // 4. LittleFS unmounten (sichert alle ausstehenden Schreibvorgänge)
    if (littlefs_mounted) {
        ESP_LOGI(TAG, "Unmounte LittleFS...");
        esp_err_t ret = esp_vfs_littlefs_unregister("storage");
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "esp_vfs_littlefs_unregister() gab Fehler zurück: %s", esp_err_to_name(ret));
        }
        littlefs_mounted = false;
        ESP_LOGI(TAG, "LittleFS unmounted");
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    ESP_LOGI(TAG, "Alle Ressourcen freigegeben");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));  // Pause, damit Log-Output gesendet wird
}

// ============================================
// Zentrale Reboot-Funktion
// ============================================
void perform_reboot(const char* reason) {
    ESP_LOGI(TAG, "Reboot wird durchgeführt: %s", reason);
    
    // WICHTIG: pulse_counter vor esp_restart() in Ring-Speicher speichern
    // (RTC-RAM wird bei esp_restart() zurückgesetzt)
    ESP_LOGI(TAG, "Speichere pulse_counter in Ring-Speicher vor Reboot...");
    write_pulse_counter_to_ring_buffer();
    
    // Ressourcen sauber beenden
    shutdown_resources();
    
    ESP_LOGI(TAG, "Starte Reboot...");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));  // Zusätzliche Verzögerung vor Reboot
    esp_restart();
}

// ============================================
// Berechne nächsten Timer-Wake-up-Zeitpunkt (Cron-ähnlich)
// ============================================
uint64_t calculate_next_wakeup_timer() {
    struct tm timeinfo;
    time_t now;
    time(&now);
    if (!localtime_r(&now, &timeinfo)) {
        ESP_LOGW(TAG, "WARNUNG: Keine Zeit-Synchronisation → verwende Fallback-Intervall");
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
    ESP_LOGI(TAG, "Aktuelle Zeit: %02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    ESP_LOGI(TAG, "Nächster Wake-up: %02d:%02d:00 (in %d Sekunden = %.2f Minuten)", 
             next_wakeup_tm.tm_hour, next_wakeup_tm.tm_min, seconds_until_wakeup, (float)seconds_until_wakeup / 60.0f);
    
    // Konvertiere zu Mikrosekunden für esp_sleep_enable_timer_wakeup()
    return (uint64_t)seconds_until_wakeup * 1000000ULL;
}

// ============================================
// Deep-Sleep mit GPIO- und Timer-Wake-up konfigurieren
// ============================================
// enable_timer: true = Timer-Wake-up aktivieren, false = nur GPIO-Wake-up (Taster)
void enter_deep_sleep_with_gpio_and_timer_wakeup(bool enable_timer = true) {
    ESP_LOGI(TAG, "Konfiguriere Deep-Sleep mit GPIO-Wake-up (Taster A)...");
    
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
        ESP_LOGI(TAG, "GPIO%d ist RTC-fähig → konfiguriere GPIO-Wake-up", gpio_num);
        
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
        ESP_LOGI(TAG, "RTC_PERIPH Domain ausgeschaltet → HOLD wird automatisch verwendet");
    
    // WICHTIG: Kurze Verzögerung, damit Pull-Up/Pull-Down stabilisiert
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Aktuellen GPIO-Zustand prüfen (für Debugging und Warnung)
        int gpio_state = rtc_gpio_get_level(gpio_num);
    ESP_LOGI(TAG, "BUTTON_A_GPIO aktueller Zustand: %s", gpio_state ? "HIGH" : "LOW");
    
    // WICHTIG: Bei Level-Mode weckt ESP32C6 sofort, wenn GPIO bereits im Wake-up-Level ist!
    // Daher prüfen und warnen, falls GPIO bereits im Wake-up-Level ist
    if ((wakeup_level == GPIO_INTR_LOW_LEVEL && gpio_state == 0) ||
        (wakeup_level == GPIO_INTR_HIGH_LEVEL && gpio_state == 1)) {
        ESP_LOGW(TAG, "WARNUNG: BUTTON_A_GPIO ist bereits im Wake-up-Level! System würde sofort wecken.");
        ESP_LOGW(TAG, "Stelle sicher, dass Taster nicht gedrückt ist, bevor Deep-Sleep startet!");
        return;  // Deep-Sleep abbrechen
    }
    
    // GPIO-Wake-up aktivieren (ESP32C6 unterstützt nur gpio_wakeup, nicht ext0/ext1)
    esp_err_t ret = esp_sleep_enable_gpio_wakeup();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FEHLER: GPIO-Wake-up-Konfiguration fehlgeschlagen: %s", esp_err_to_name(ret));
            ESP_LOGW(TAG, "Deep-Sleep wird ohne GPIO-Wake-up gestartet!");
    } else {
        // GPIO als Wake-up-Source setzen: Level basierend auf GPIO-Modus
        // ESP32C6 unterstützt NUR Level-Mode (LOW_LEVEL oder HIGH_LEVEL)
        ret = gpio_wakeup_enable(gpio_num, wakeup_level);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "FEHLER: gpio_wakeup_enable fehlgeschlagen: %s", esp_err_to_name(ret));
            ESP_LOGE(TAG, "Fehler-Code: %s", esp_err_to_name(ret));
        } else {
                gpio_wakeup_configured = true;
                ESP_LOGI(TAG, "GPIO-Wake-up konfiguriert: Taster A (BUTTON_A_GPIO) - %s", level_name);
            ESP_LOGI(TAG, "GPIO-Modus: %s", 
                     gpio_mode == INPUT_PULLUP ? "INPUT_PULLUP" : 
                     (gpio_mode == INPUT_PULLDOWN ? "INPUT_PULLDOWN" : "INPUT"));
            ESP_LOGI(TAG, "HOLD-Funktion aktiv: Pull-Up/Pull-Down wird während Deep-Sleep gehalten");
        }
        }
    } else {
        // Nicht-RTC-Pin: GPIO-Wake-up nicht möglich
        ESP_LOGW(TAG, "WARNUNG: GPIO%d ist NICHT RTC-fähig (nur GPIOs 0-7)", gpio_num);
        ESP_LOGW(TAG, "GPIO-Wake-up (Taster A) kann nicht konfiguriert werden!");
        ESP_LOGW(TAG, "Nur Timer-Wake-up möglich (falls aktiviert)");
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
                ESP_LOGE(TAG, "FEHLER: Timer-Wake-up-Konfiguration fehlgeschlagen: %s", esp_err_to_name(ret));
            } else {
                timer_activated = true;
                // Berechne nächste Wake-up-Zeit für Ausgabe
                struct tm timeinfo;
                time_t now;
                time(&now);
                if (localtime_r(&now, &timeinfo)) {
                    time_t current_time = mktime(&timeinfo);
                    time_t next_wakeup_time = current_time + (wakeup_time_us / 1000000ULL);
                    localtime_r(&next_wakeup_time, &next_wakeup_tm);
                }
            }
        }
    } else {
        ESP_LOGI(TAG, "Timer-Wake-up DEAKTIVIERT (nur GPIO-Wake-up aktiv)");
        ESP_LOGI(TAG, "Nur manueller Wake-up über Taster A möglich");
    }
    
    // Prüfen, ob mindestens eine Wake-up-Quelle konfiguriert wurde
    // WICHTIG: Ohne Wake-up-Quelle würde ESP32C6 nicht mehr aufwachen!
    if (!gpio_wakeup_configured && !timer_activated) {
        ESP_LOGE(TAG, "FEHLER: Keine Wake-up-Quelle konfiguriert!");
        ESP_LOGE(TAG, "Deep-Sleep wird ABGEBROCHEN - System bleibt aktiv");
        ESP_LOGE(TAG, "Mögliche Ursachen:");
        if (!is_rtc_gpio) {
            ESP_LOGE(TAG, "  - GPIO ist nicht RTC-fähig (nur GPIOs 0-7)");
        }
        if (!enable_timer) {
            ESP_LOGE(TAG, "  - Timer-Wake-up ist deaktiviert (Akku-Schutz)");
        } else if (!timer_activated) {
            ESP_LOGE(TAG, "  - Timer-Wake-up konnte nicht aktiviert werden");
        }
        return;  // Funktion beenden, Ressourcen bleiben aktiv
    }
    
    // LED ausschalten (HP-Core geht in Deep-Sleep)
    gpio_set_level((gpio_num_t)LED_BUILTIN_GPIO, LED_OFF);
    ESP_LOGI(TAG, "Interne LED ausgeschaltet (Deep-Sleep)");
    
    // Ressourcen sauber beenden (nur wenn Wake-up konfiguriert wurde)
    shutdown_resources();
    
    // Finale Deep-Sleep-Ausgabe (dynamisch basierend auf aktivierten Wake-up-Quellen)
    ESP_LOGI(TAG, "=== Gehe in Deep-Sleep ===");
    ESP_LOGI(TAG, "Wake-up möglich durch:");
    if (gpio_wakeup_configured) {
        ESP_LOGI(TAG, "  - Taster A (GPIO-Wake-up)");
    } else {
        ESP_LOGI(TAG, "  - Taster A: NICHT verfügbar (GPIO nicht RTC-fähig)");
    }
    if (timer_activated) {
        ESP_LOGI(TAG, "  - Timer (Cron-Intervall): %02d:%02d:00 (in %llu Sekunden = %.2f Minuten)",
                 next_wakeup_tm.tm_hour, next_wakeup_tm.tm_min,
                 wakeup_time_us / 1000000ULL, (float)wakeup_time_us / 60000000.0f);
    } else {
        if (enable_timer) {
            ESP_LOGI(TAG, "  - Timer: FEHLER bei Aktivierung");
        } else {
            ESP_LOGI(TAG, "  - Timer: DEAKTIVIERT (Akku-Schutz)");
        }
    }
    fflush(stdout);
    
    esp_deep_sleep_start();
    // Ab hier wird Code nicht mehr ausgeführt
}

// ============================================
// Config.json laden
// ============================================
bool load_config() {
    // Prüfen, ob Config bereits geladen ist (z.B. aus RTC-RAM nach Wake-up)
    if (config_rtc.config_loaded) {
        ESP_LOGI(TAG, "Config bereits geladen (aus RTC-RAM) → kein erneutes Laden nötig");
        return true;
    }
    
    if (!mount_littlefs()) {
        return false;
    }
    
    FILE* configFile = fopen("/spiffs/config.json", "r");
    if (!configFile) {
        ESP_LOGE(TAG, "config.json nicht gefunden");
        return false;
    }
    
    // Dateigröße ermitteln
    fseek(configFile, 0, SEEK_END);
    long size = ftell(configFile);
    fseek(configFile, 0, SEEK_SET);
    
    if (size > 1024) {
        ESP_LOGE(TAG, "config.json zu groß (%ld Bytes)", size);
        fclose(configFile);
        return false;
    }
    
    // Datei in Puffer lesen (ArduinoJson kann nicht direkt mit FILE* arbeiten)
    char* buffer = (char*)malloc(size + 1);
    if (!buffer) {
        ESP_LOGE(TAG, "Speicher für config.json Puffer fehlt");
        fclose(configFile);
        return false;
    }
    size_t read_size = fread(buffer, 1, size, configFile);
    fclose(configFile);
    buffer[read_size] = '\0';
    
    // JSON parsen
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, buffer);
    free(buffer);
    
    if (error) {
        ESP_LOGE(TAG, "JSON Parse Fehler: %s", error.c_str());  // ArduinoJson DeserializationError hat c_str()
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
            ESP_LOGW(TAG, "WARNUNG: Mehr als 2 WiFi-Credentials gefunden, nur die ersten 2 werden verwendet");
        }
        ESP_LOGI(TAG, "WiFi-Credentials geladen: %d Paar(e)", config_rtc.wifi_count);
    }
    
    config_rtc.config_loaded = true;
    
    ESP_LOGI(TAG, "Config.json erfolgreich geladen");
    return true;
}

// ============================================
// Config speichern (RTC-RAM und config.json)
// ============================================
// Rückgabewert: true = erfolgreich, false = Fehler
// wifi_credentials_changed wird auf true gesetzt, wenn WiFi-Credentials geändert wurden
bool save_config(JsonDocument& doc, bool* wifi_credentials_changed = nullptr, char errorMessage[] = nullptr) {
    if (wifi_credentials_changed != nullptr) {
        *wifi_credentials_changed = false;
    }
    if (errorMessage != nullptr) {
        errorMessage[0] = '\0';
    }
    
    if (!mount_littlefs()) {
        ESP_LOGE(TAG, "Fehler: LittleFS nicht gemountet");
        if (errorMessage != nullptr) {
            strncpy(errorMessage, "Fehler: LittleFS nicht gemountet", 255);
        }
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
    
    // Werte in RTC Memory speichern mit Validierung
    if (doc["hostname"].is<const char*>()) {
        const char* hostname = doc["hostname"];
        if (strlen(hostname) > 0 && strlen(hostname) < sizeof(config_rtc.hostname)) {
            strncpy(config_rtc.hostname, hostname, sizeof(config_rtc.hostname) - 1);
            config_rtc.hostname[sizeof(config_rtc.hostname) - 1] = '\0';
        } else {
            ESP_LOGE(TAG, "FEHLER: Hostname ungültig (leer oder zu lang)");
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: Hostname ungültig (leer oder zu lang)", 255);
            }
            return false;
        }
    }
    
    if (doc["adminpass"].is<const char*>()) {
        const char* adminpass = doc["adminpass"];
        if (strlen(adminpass) > 0 && strlen(adminpass) < sizeof(config_rtc.adminpass)) {
            strncpy(config_rtc.adminpass, adminpass, sizeof(config_rtc.adminpass) - 1);
            config_rtc.adminpass[sizeof(config_rtc.adminpass) - 1] = '\0';
        } else {
            ESP_LOGE(TAG, "FEHLER: Admin-Passwort ungültig (leer oder zu lang)");
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: Admin-Passwort ungültig (leer oder zu lang)", 255);
            }
            return false;
        }
    }
    
    if (doc["wakeup_minutes"].is<uint8_t>()) {
        uint8_t wakeup_minutes = doc["wakeup_minutes"].as<uint8_t>();
        if (wakeup_minutes >= 1 && wakeup_minutes <= 60) {
            config_rtc.wakeup_minutes = wakeup_minutes;
        } else {
            ESP_LOGE(TAG, "FEHLER: Wake-up Intervall ungültig (%d, muss zwischen 1-60 sein)", wakeup_minutes);
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: Wake-up Intervall ungültig (muss zwischen 1-60 sein)", 255);
            }
            return false;
        }
    }
    
    if (doc["transfer_minutes"].is<uint8_t>()) {
        uint8_t transfer_minutes = doc["transfer_minutes"].as<uint8_t>();
        if (transfer_minutes >= 1 && transfer_minutes <= 60) {
            config_rtc.transfer_minutes = transfer_minutes;
        } else {
            ESP_LOGE(TAG, "FEHLER: Transfer Intervall ungültig (%d, muss zwischen 1-60 sein)", transfer_minutes);
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: Transfer Intervall ungültig (muss zwischen 1-60 sein)", 255);
            }
            return false;
        }
    }
    
    if (doc["adc_voltage_offset"].is<float>()) {
        float adc_voltage_offset = doc["adc_voltage_offset"].as<float>();
        // Keine strenge Begrenzung, aber prüfe auf sinnvolle Werte (z.B. -10V bis +10V)
        if (adc_voltage_offset >= -10.0f && adc_voltage_offset <= 10.0f) {
            config_rtc.adc_voltage_offset = adc_voltage_offset;
        } else {
            ESP_LOGE(TAG, "FEHLER: ADC Spannungs-Offset ungültig (%.2f, muss zwischen -10.0 und +10.0 sein)", adc_voltage_offset);
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: ADC Spannungs-Offset ungültig (muss zwischen -10.0 und +10.0 sein)", 255);
            }
            return false;
        }
    }
    
    if (doc["ntp_server"].is<const char*>()) {
        const char* ntp_server = doc["ntp_server"];
        if (strlen(ntp_server) > 0 && strlen(ntp_server) < sizeof(config_rtc.ntp_server)) {
            strncpy(config_rtc.ntp_server, ntp_server, sizeof(config_rtc.ntp_server) - 1);
            config_rtc.ntp_server[sizeof(config_rtc.ntp_server) - 1] = '\0';
        } else {
            ESP_LOGE(TAG, "FEHLER: NTP-Server ungültig (leer oder zu lang)");
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: NTP-Server ungültig (leer oder zu lang)", 255);
            }
            return false;
        }
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
                
                // Nur nicht-leere Sets speichern (mindestens SSID muss vorhanden sein)
                if (strlen(ssid) > 0) {
                    if (strlen(ssid) >= sizeof(config_rtc.wifi_credentials[i].ssid)) {
                        ESP_LOGE(TAG, "FEHLER: WiFi SSID zu lang (max. %d Zeichen)", sizeof(config_rtc.wifi_credentials[i].ssid) - 1);
                        if (errorMessage != nullptr) {
                            strncpy(errorMessage, "Fehler: WiFi SSID zu lang (max. 31 Zeichen)", 255);
                        }
                        return false;
                    }
                    if (strlen(password) >= sizeof(config_rtc.wifi_credentials[i].password)) {
                        ESP_LOGE(TAG, "FEHLER: WiFi Passwort zu lang (max. %d Zeichen)", sizeof(config_rtc.wifi_credentials[i].password) - 1);
                        if (errorMessage != nullptr) {
                            strncpy(errorMessage, "Fehler: WiFi Passwort zu lang (max. 63 Zeichen)", 255);
                        }
                        return false;
                    }
                    
                    strncpy(config_rtc.wifi_credentials[i].ssid, ssid, sizeof(config_rtc.wifi_credentials[i].ssid) - 1);
                    config_rtc.wifi_credentials[i].ssid[sizeof(config_rtc.wifi_credentials[i].ssid) - 1] = '\0';
                    
                    strncpy(config_rtc.wifi_credentials[i].password, password, sizeof(config_rtc.wifi_credentials[i].password) - 1);
                    config_rtc.wifi_credentials[i].password[sizeof(config_rtc.wifi_credentials[i].password) - 1] = '\0';
                    
                    config_rtc.wifi_count++;
                }
            }
        }
    }
    
    // Validierung: Mindestens ein WiFi-Set muss vorhanden sein
    if (config_rtc.wifi_count == 0) {
        ESP_LOGE(TAG, "FEHLER: Mindestens ein WiFi-Set (SSID) muss angegeben werden");
        if (errorMessage != nullptr) {
            strncpy(errorMessage, "Fehler: Mindestens ein WiFi-Set (SSID) muss angegeben werden", 255);
        }
        return false;
    }
    
    // Config in config.json speichern
    // WICHTIG: Wir müssen ein neues JSON-Dokument erstellen, da das eingehende doc
    // möglicherweise nicht alle Felder enthält (z.B. wenn nur WiFi-Credentials geändert wurden)
    FILE* configFile = fopen("/spiffs/config.json", "w");
    if (!configFile) {
        ESP_LOGE(TAG, "Fehler: config.json konnte nicht zum Schreiben geöffnet werden");
        if (errorMessage != nullptr) {
            strncpy(errorMessage, "Fehler: config.json konnte nicht zum Schreiben geöffnet werden", 255);
        }
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
    // ArduinoJson serializeJsonPretty funktioniert nicht direkt mit FILE*, daher Buffer verwenden
    char json_buffer[2048];
    size_t json_len = serializeJsonPretty(newDoc, json_buffer, sizeof(json_buffer));
    if (json_len > 0) {
        fwrite(json_buffer, 1, json_len, configFile);
    }
    fclose(configFile);
    
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
            ESP_LOGI(TAG, "WiFi-Credentials wurden geändert");
        }
    }
    
    ESP_LOGI(TAG, "Config erfolgreich gespeichert (RTC-RAM und config.json)");
    return true;
}

// ============================================
// WiFi-Event-Handler (ESP-IDF)
// ============================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi Station gestartet");
                // esp_wifi_connect() wird manuell nach WiFi-Scan aufgerufen (in connect_wifi())
                break;
            case WIFI_EVENT_STA_CONNECTED: {
                wifi_event_sta_connected_t* event = (wifi_event_sta_connected_t*) event_data;
                ESP_LOGI(TAG, "Verbunden mit SSID: %s", event->ssid);
                break;
            }
            case WIFI_EVENT_STA_DISCONNECTED:
                wifi_connected = false;
                ESP_LOGI(TAG, "WiFi getrennt");
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP: {
                ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
                wifi_ip_info = event->ip_info;
                wifi_connected = true;
                ESP_LOGI(TAG, "IP erhalten: " IPSTR, IP2STR(&wifi_ip_info.ip));
                break;
            }
            default:
                break;
        }
    }
}

// Statische Variablen für Event-Handler-Instanzen
static esp_event_handler_instance_t instance_any_id = NULL;
static esp_event_handler_instance_t instance_got_ip = NULL;

// ============================================
// WiFi-Initialisierung (ESP-IDF)
// ============================================
bool init_wifi() {
    if (wifi_initialized) {
        return true;  // Bereits initialisiert
    }
    
    // Event-Loop und Netif initialisieren (nur einmal)
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi-Initialisierung fehlgeschlagen: %s", esp_err_to_name(ret));
        return false;
    }

    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    // Event-Handler registrieren (nur einmal)
    if (instance_any_id == NULL) {
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, NULL, &instance_any_id);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &wifi_event_handler, NULL, &instance_got_ip);
    }

    wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi initialisiert");
    return true;
}

// ============================================
// WiFi-Verbindung mit höchster RSSI
// ============================================
bool connect_wifi() {
    if (config_rtc.wifi_count == 0) {
        ESP_LOGE(TAG, "Keine WiFi-Credentials verfügbar");
        return false;
    }
    
    // WiFi initialisieren (falls noch nicht geschehen)
    if (!init_wifi()) {
        return false;
    }
    
    // WiFi-Modus auf Station setzen
    esp_wifi_set_mode(WIFI_MODE_STA);
    
    // WiFi starten (erforderlich für Scan)
    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi-Start fehlgeschlagen: %s", esp_err_to_name(ret));
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));  // Kurze Verzögerung für WiFi-Initialisierung
    
    // Hostname setzen (für DHCP-Server)
    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) {
        esp_netif_set_hostname(sta_netif, config_rtc.hostname);
        ESP_LOGI(TAG, "Hostname gesetzt: %s", config_rtc.hostname);
    }
    
    // WiFi-Scan durchführen
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
    };
    ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi-Scan fehlgeschlagen: %s", esp_err_to_name(ret));
        return false;
    }
    
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ESP_LOGE(TAG, "Keine Netzwerke gefunden");
        return false;
    }
    
    wifi_ap_record_t ap_records[ap_count];
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    
    // Bestes Netzwerk finden (höchste RSSI)
    int best_rssi = -1000;
    const char* best_ssid = NULL;
    const char* best_password = NULL;
    
    for (uint8_t i = 0; i < config_rtc.wifi_count; i++) {
        const char* ssid = config_rtc.wifi_credentials[i].ssid;
        const char* password = config_rtc.wifi_credentials[i].password;
        
        for (uint16_t j = 0; j < ap_count; j++) {
            // SSID vergleichen (C-String)
            if (strcmp((const char*)ap_records[j].ssid, ssid) == 0) {
                int rssi = ap_records[j].rssi;
                if (rssi > best_rssi) {
                    best_rssi = rssi;
                    best_ssid = ssid;
                    best_password = password;
                    ap_info = ap_records[j];  // Für späteren Zugriff speichern
                }
            }
        }
    }
    
    if (best_ssid == NULL) {
        ESP_LOGE(TAG, "Kein bekanntes Netzwerk gefunden");
        return false;
    }
    
    // Verbindung herstellen
    ESP_LOGI(TAG, "Verbinde mit: %s (RSSI: %d dBm)", best_ssid, best_rssi);
    
    wifi_config_t wifi_config = {};
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    strncpy((char*)wifi_config.sta.ssid, best_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, best_password, sizeof(wifi_config.sta.password) - 1);
    
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();
    
    // Warte auf Verbindung (über Event-Handler)
    int attempts = 0;
    while (!wifi_connected && attempts < 40) {  // 20 Sekunden (40 * 500ms)
        vTaskDelay(pdMS_TO_TICKS(500));
        attempts++;
    }
    
    if (wifi_connected) {
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&wifi_ip_info.ip));
        ESP_LOGI(TAG, "WiFi verbunden! IP: %s", ip_str);
        return true;
    } else {
        ESP_LOGE(TAG, "WiFi-Verbindung fehlgeschlagen");
        return false;
    }
}

// ============================================
// Access Point starten (offenes WLAN)
// ============================================
bool start_access_point() {
    ESP_LOGI(TAG, "Starte Access Point...");
    
    // WiFi initialisieren (falls noch nicht geschehen)
    if (!init_wifi()) {
        return false;
    }
    
    // WiFi stoppen (falls es läuft), bevor Modus geändert wird
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // WiFi-Modus auf AP setzen
    esp_wifi_set_mode(WIFI_MODE_AP);
    
    // AP-Konfiguration
    // SSID kopieren (max. 32 Zeichen)
    size_t ssid_len = strlen(config_rtc.hostname);
    if (ssid_len > 31) {
        ssid_len = 31;
        ESP_LOGW(TAG, "SSID zu lang, gekürzt auf: %.*s", (int)ssid_len, config_rtc.hostname);
    }
    
    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = (uint8_t)ssid_len,  // Expliziter Cast zu uint8_t (max. 31 Zeichen)
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };
    
    strncpy((char*)ap_config.ap.ssid, config_rtc.hostname, ssid_len);
    ap_config.ap.ssid[ssid_len] = '\0';
    ap_config.ap.ssid_len = (uint8_t)ssid_len;  // Expliziter Cast zu uint8_t
    
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AP-Konfiguration fehlgeschlagen: %s", esp_err_to_name(ret));
        return false;
    }
    
    // IP-Konfiguration für AP
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_ip_info_t ip_info;
        IP4_ADDR(&ip_info.ip, AP_IP_ADDRESS_1, AP_IP_ADDRESS_2, AP_IP_ADDRESS_3, AP_IP_ADDRESS_4);
        IP4_ADDR(&ip_info.gw, AP_IP_ADDRESS_1, AP_IP_ADDRESS_2, AP_IP_ADDRESS_3, AP_IP_ADDRESS_4);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        esp_netif_dhcps_stop(ap_netif);
        esp_netif_set_ip_info(ap_netif, &ip_info);
        esp_netif_dhcps_start(ap_netif);
    }
    
    // WiFi starten
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AP-Start fehlgeschlagen: %s", esp_err_to_name(ret));
        return false;
    }
    
    ESP_LOGI(TAG, "Access Point gestartet: %s", config_rtc.hostname);
    ESP_LOGI(TAG, "AP IP: %d.%d.%d.%d", AP_IP_ADDRESS_1, AP_IP_ADDRESS_2, AP_IP_ADDRESS_3, AP_IP_ADDRESS_4);
    ESP_LOGI(TAG, "WLAN ist offen (kein Passwort)");
    return true;
}

// ============================================
// NTP-Zeitsynchronisation (ESP-IDF)
// ============================================
// Callback für Zeit-Synchronisation
static void time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "Zeit synchronisiert");
}

bool sync_ntp_time() {
    if (!wifi_connected) {
        ESP_LOGE(TAG, "NTP-Sync: WiFi nicht verbunden");
        // Fehlgeschlagen: Marker erhöhen (wenn > 0) oder bei 0 bleiben
        if (ntp_sync_marker > 0) {
            ntp_sync_marker++;
            if (ntp_sync_marker > 255) {
                ntp_sync_marker = 0;  // Nach 255 fehlgeschlagenen Versuchen auf 0 zurücksetzen
            }
        }
        return false;
    }
    
    ESP_LOGI(TAG, "NTP-Sync: Verbinde mit %s...", config_rtc.ntp_server);
    
    // Zeitzone explizit auf UTC setzen (für Datenlogger wichtig - keine Sommer/Winterzeit-Sprünge!)
    setenv("TZ", "UTC", 1);
    tzset();
    
    // NTP-Konfiguration (ESP-IDF SNTP)
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, config_rtc.ntp_server);
    // Hinweis: sntp_set_time_sync_notification_cb() existiert nicht in ESP-IDF 5.x
    // Die Callback-Registrierung erfolgt über sntp_set_time_sync_notification_cb() in älteren Versionen,
    // aber in ESP-IDF 5.x wird die Zeit-Synchronisation über SNTP-Events gehandhabt
    sntp_init();
    
    // Warte auf Zeit-Synchronisation
    struct tm timeinfo;
    uint64_t start_time_us = esp_timer_get_time();
    uint64_t timeout_us = NTP_TIMEOUT_MS * 1000ULL;
    
    time_t now;
    bool synced = false;
    while ((esp_timer_get_time() - start_time_us) < timeout_us) {
        time(&now);
        if (localtime_r(&now, &timeinfo) && timeinfo.tm_year > (2016 - 1900)) {
            // Zeit ist synchronisiert (tm_year > 2016 bedeutet, dass Zeit gesetzt wurde)
            synced = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    if (synced) {
        // Sync erfolgreich: Marker auf 1 setzen
        ntp_sync_marker = 1;
        ESP_LOGI(TAG, "NTP-Sync erfolgreich (UTC): %04d-%02d-%02d %02d:%02d:%02d UTC",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        ESP_LOGI(TAG, "NTP-Sync-Marker: %d (1=erfolgreich, 2+=%d fehlgeschlagene Versuche)",
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
        ESP_LOGE(TAG, "NTP-Sync fehlgeschlagen (Timeout), Marker: %d", ntp_sync_marker);
        return false;
    }
}

// ============================================
// Template-Processor (C-String-Version für ESP-IDF)
// ============================================
const char* processor_get_value(const char* var) {
    static char buffer[512];  // Statischer Buffer für Rückgabewerte
    
    // if-else if-Kette für alle Variablen (alphabetisch sortiert)
    if (strcmp(var, "adc_value") == 0) {
        snprintf(buffer, sizeof(buffer), "%lu", battery_adc_mv);
        return buffer;
    }
    if (strcmp(var, "adminpass") == 0) {
        return config_rtc.adminpass;
    }
    if (strcmp(var, "adc_voltage_offset") == 0) {
        snprintf(buffer, sizeof(buffer), "%.2f", config_rtc.adc_voltage_offset);
        return buffer;
    }
    if (strcmp(var, "battery_percent") == 0) {
        snprintf(buffer, sizeof(buffer), "%d", battery_percent);
        return buffer;
    }
    if (strcmp(var, "battery_voltage") == 0) {
        snprintf(buffer, sizeof(buffer), "%.2f", battery_voltage);
        return buffer;
    }
    if (strcmp(var, "build_date") == 0) {
        return SKETCHCOMPILE;
    }
    if (strcmp(var, "currentWifiData") == 0) {
        // Aktuelle WiFi-Credentials für Vergleich
        static char json_buffer[256];
        strcpy(json_buffer, "[");
        if (wifi_connected) {
            // Finde passendes Credential
            for (uint8_t i = 0; i < config_rtc.wifi_count; i++) {
                if (strcmp((const char*)ap_info.ssid, config_rtc.wifi_credentials[i].ssid) == 0) {
                    strcat(json_buffer, "{");
                    strcat(json_buffer, "\"ssid\":\"");
                    strcat(json_buffer, (const char*)ap_info.ssid);
                    strcat(json_buffer, "\",\"password\":\"");
                    strcat(json_buffer, config_rtc.wifi_credentials[i].password);
                    strcat(json_buffer, "\"");
                    strcat(json_buffer, "}");
                    break;
                }
            }
        }
        strcat(json_buffer, "]");
        return json_buffer;
    }
    if (strcmp(var, "hostname") == 0) {
        return config_rtc.hostname;
    }
    if (strcmp(var, "ntp_server") == 0) {
        return config_rtc.ntp_server;
    }
    if (strcmp(var, "nv_magic_key") == 0) {
        snprintf(buffer, sizeof(buffer), "%lu (0x%08lX)", RING_BUFFER_VERSION, RING_BUFFER_VERSION);
        return buffer;
    }
    if (strcmp(var, "project_name") == 0) {
        return PROJECT_NAME;
    }
    if (strcmp(var, "project_version") == 0) {
        return PROJECT_VERSION;
    }
    if (strcmp(var, "pulse_counter") == 0) {
        // Formatierung: 7 Stellen mit führenden Nullen (99999.99 = 9999999)
        snprintf(buffer, sizeof(buffer), "%07lu", pulse_counter);
        return buffer;
    }
    if (strcmp(var, "pulse_counter_left") == 0) {
        // Linke 5 Stellen für CSS-Formatierung (Vorkommastellen)
        snprintf(buffer, sizeof(buffer), "%05lu", pulse_counter / 100);
        return buffer;
    }
    if (strcmp(var, "pulse_counter_right") == 0) {
        // Rechte 2 Stellen für CSS-Formatierung (Nachkommastellen)
        snprintf(buffer, sizeof(buffer), "%02lu", pulse_counter % 100);
        return buffer;
    }
    if (strcmp(var, "system_time") == 0) {
        struct tm timeinfo;
        time_t now;
        time(&now);
        if (localtime_r(&now, &timeinfo)) {
            snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d UTC",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            return buffer;
        } else {
            return "Nicht synchronisiert";
        }
    }
    if (strcmp(var, "transfer_minutes") == 0) {
        snprintf(buffer, sizeof(buffer), "%d", config_rtc.transfer_minutes);
        return buffer;
    }
    if (strcmp(var, "wakeup_count") == 0) {
        snprintf(buffer, sizeof(buffer), "%d", wakeupCount);
        return buffer;
    }
    if (strcmp(var, "wakeup_minutes") == 0) {
        snprintf(buffer, sizeof(buffer), "%d", config_rtc.wakeup_minutes);
        return buffer;
    }
    if (strcmp(var, "wifiCredentialsData") == 0) {
        // WiFi-Credentials als JSON für JavaScript
        static char json_buffer[512];
        strcpy(json_buffer, "[");
        for (uint8_t i = 0; i < config_rtc.wifi_count; i++) {
            if (i > 0) strcat(json_buffer, ",");
            strcat(json_buffer, "{");
            strcat(json_buffer, "\"ssid\":\"");
            strcat(json_buffer, config_rtc.wifi_credentials[i].ssid);
            strcat(json_buffer, "\",\"password\":\"");
            strcat(json_buffer, config_rtc.wifi_credentials[i].password);
            strcat(json_buffer, "\"");
            strcat(json_buffer, "}");
        }
        strcat(json_buffer, "]");
        return json_buffer;
    }
    if (strcmp(var, "wifi_info_style") == 0) {
        // WiFi-Info Sichtbarkeit: "display:block;" wenn verbunden, "display:none;" wenn nicht verbunden
        return wifi_connected ? "display:block;" : "display:none;";
    }
    if (strcmp(var, "wifi_ip") == 0) {
        if (wifi_connected) {
            snprintf(buffer, sizeof(buffer), IPSTR, IP2STR(&wifi_ip_info.ip));
            return buffer;
        }
        return "-";
    }
    if (strcmp(var, "wifi_rssi") == 0) {
        if (wifi_connected) {
            snprintf(buffer, sizeof(buffer), "%d", ap_info.rssi);
            return buffer;
        }
        return "-";
    }
    if (strcmp(var, "wifi_ssid") == 0) {
        // Zeige aktuell verbundenes SSID (falls verbunden)
        if (wifi_connected) {
            return (const char*)ap_info.ssid;
        }
        return "Nicht verbunden";
    }
    if (strcmp(var, "wifi_status") == 0) {
        return wifi_connected ? "Verbunden" : "Nicht verbunden";
    }
    
    // Unbekannte Variable - leeren String zurückgeben
    return "";
}

// ============================================
// Template-Datei verarbeiten (ESP-IDF)
// ============================================
esp_err_t process_template_file(const char* filepath, httpd_req_t *req) {
    FILE* file = fopen(filepath, "r");
    if (!file) {
        ESP_LOGE(TAG, "Datei nicht gefunden: %s", filepath);
        return ESP_FAIL;
    }
    
    char line[512];
    char var_name[128];
    const char* var_value;
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    
    while (fgets(line, sizeof(line), file)) {
        char* processed = (char*)malloc(strlen(line) * 4);
        if (!processed) {
            fclose(file);
            return ESP_ERR_NO_MEM;
        }
        processed[0] = '\0';
        
        char* pos = line;
        while (*pos) {
            // Suche nach Start-Platzhalter (TEMPLATE_PLACEHOLDER = '`')
            char* start = strchr(pos, TEMPLATE_PLACEHOLDER);
            if (!start) {
                strcat(processed, pos);
                break;
            }
            
            // Text vor Platzhalter
            size_t before_len = start - pos;
            if (before_len > 0) {
                char temp[512];
                strncpy(temp, pos, before_len);
                temp[before_len] = '\0';
                strcat(processed, temp);
            }
            
            // Variable-Name extrahieren
            char* end = strchr(start + 1, TEMPLATE_PLACEHOLDER);
            if (!end) {
                strcat(processed, start);
                break;
            }
            
            size_t var_len = end - start - 1;
            if (var_len > 0 && var_len < sizeof(var_name) - 1) {
                strncpy(var_name, start + 1, var_len);
                var_name[var_len] = '\0';
                
                var_value = processor_get_value(var_name);
                strcat(processed, var_value);
            }
            
            pos = end + 1;
        }
        
        httpd_resp_sendstr_chunk(req, processed);
        free(processed);
    }
    
    fclose(file);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// ============================================
// Base64-Dekodierung (einfache Implementierung)
// ============================================
static int base64_decode(const char* input, char* output, size_t output_len) {
    const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int in_len = strlen(input);
    int i = 0, j = 0;
    int in = 0;
    unsigned char char_array_4[4], char_array_3[3];
    
    while (in_len-- && (input[in] != '=') && j < (int)output_len - 1) {
        char_array_4[i++] = input[in]; in++;
        if (i == 4) {
            for (i = 0; i < 4; i++) {
                char_array_4[i] = strchr(base64_chars, char_array_4[i]) - base64_chars;
            }
            
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
            
            for (i = 0; (i < 3); i++) {
                output[j++] = char_array_3[i];
            }
            i = 0;
        }
    }
    
    if (i) {
        for (int k = i; k < 4; k++) {
            char_array_4[k] = 0;
        }
        for (int k = 0; k < 4; k++) {
            char_array_4[k] = strchr(base64_chars, char_array_4[k]) - base64_chars;
        }
        
        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
        
        for (int k = 0; (k < i - 1); k++) {
            output[j++] = char_array_3[k];
        }
    }
    
    output[j] = '\0';
    return j;
}

// ============================================
// Basic Auth prüfen (ESP-IDF)
// ============================================
bool check_basic_auth(httpd_req_t *req, const char* username, const char* password) {
    size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (auth_len == 0) {
        return false;
    }
    
    char auth_header[auth_len + 1];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        return false;
    }
    
    // Format: "Basic base64(username:password)"
    if (strncmp(auth_header, "Basic ", 6) != 0) {
        return false;
    }
    
    // Base64 dekodieren
    char decoded[64];
    int decoded_len = base64_decode(auth_header + 6, decoded, sizeof(decoded));
    if (decoded_len <= 0) {
        return false;
    }
    
    // Erwartetes Format: "username:password"
    char expected[128];
    snprintf(expected, sizeof(expected), "%s:%s", username, password);
    
    return (strcmp(decoded, expected) == 0);
}

// ============================================
// Web-Server Handler (ESP-IDF)
// ============================================

// Handler für /ping
static esp_err_t ping_handler(httpd_req_t *req) {
    last_web_activity_us = esp_timer_get_time();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "pong", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler für /version
static esp_err_t version_handler(httpd_req_t *req) {
    last_web_activity_us = esp_timer_get_time();
    char version[80];
    snprintf(version, sizeof(version), "v%s (Build %s) %s", 
             PROJECT_VERSION, SKETCHCOMPILE, PROJECT_NAME);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, version, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler für /reading
static esp_err_t reading_handler(httpd_req_t *req) {
    last_web_activity_us = esp_timer_get_time();
    
    // Format: XXXXX.XX (5 Vorkommastellen + Punkt + 2 Nachkommastellen)
    uint32_t vorkommastellen = pulse_counter / 100;
    uint32_t nachkommastellen = pulse_counter % 100;
    
    char reading[9];
    snprintf(reading, sizeof(reading), "%05lu.%02lu", vorkommastellen, nachkommastellen);
    
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, reading, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler für /index.html (GET und HEAD)
static esp_err_t index_handler(httpd_req_t *req) {
    last_web_activity_us = esp_timer_get_time();
    
    // Für HEAD-Requests nur Header senden, kein Body
    if (req->method == HTTP_HEAD) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    // ADC-Werte bei jedem Seitenaufruf neu messen (für Auto-Refresh)
    battery_adc_mv = read_adc_median_mv();
    battery_voltage = (float)battery_adc_mv / 1000.0f * VOLTAGE_DIVIDER_RATIO + config_rtc.adc_voltage_offset;
    battery_percent = VOLTAGE_TO_PERCENT(battery_voltage);
    
    // Template-Datei verarbeiten
    return process_template_file("/spiffs/index.html", req);
}

// Handler für /config (GET)
static esp_err_t config_get_handler(httpd_req_t *req) {
    // Basic Auth prüfen
    if (!check_basic_auth(req, "admin", config_rtc.adminpass)) {
        // WICHTIG: WWW-Authenticate Header VOR Status setzen (Browser-Kompatibilität)
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"GasOMeterKonfiguration\"");
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }
    
    last_web_activity_us = esp_timer_get_time();
    
    // Template-Datei verarbeiten
    return process_template_file("/spiffs/config.html", req);
}

// Hilfsfunktion: POST-Daten lesen
static int read_post_data(httpd_req_t *req, char* buffer, size_t max_len) {
    int total_len = req->content_len;
    if (total_len >= max_len) {
        return -1;  // Zu groß
    }
    
    int remaining = total_len;
    int received = 0;
    
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buffer + received, remaining);
        if (ret <= 0) {
            return -1;  // Fehler
        }
        received += ret;
        remaining -= ret;
    }
    
    buffer[received] = '\0';
    return received;
}

// Hilfsfunktion: URL-dekodieren (vereinfacht)
static void url_decode(char* str) {
    char* src = str;
    char* dst = str;
    
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            int value;
            sscanf(src + 1, "%2x", &value);
            *dst++ = (char)value;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// Hilfsfunktion: Query-Parameter extrahieren
static bool get_query_param(httpd_req_t *req, const char* key, char* value, size_t max_len) {
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) {
        return false;
    }
    
    char query[query_len + 1];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    
    // Parameter suchen
    char key_with_eq[64];
    snprintf(key_with_eq, sizeof(key_with_eq), "%s=", key);
    char* param_start = strstr(query, key_with_eq);
    if (!param_start) {
        return false;
    }
    
    param_start += strlen(key_with_eq);
    char* param_end = strchr(param_start, '&');
    size_t param_len = param_end ? (param_end - param_start) : strlen(param_start);
    
    if (param_len >= max_len) {
        return false;
    }
    
    strncpy(value, param_start, param_len);
    value[param_len] = '\0';
    url_decode(value);
    return true;
}

// Hilfsfunktion: POST-Parameter extrahieren (wie Arduino's hasParam/getParam mit true)
// Unterstützt sowohl multipart/form-data als auch application/x-www-form-urlencoded
static bool get_post_param(const char* post_data, size_t post_len, const char* key, char* value, size_t max_len) {
    // Prüfe ob multipart/form-data (hat "Content-Disposition" und "name=")
    bool is_multipart = (strstr(post_data, "Content-Disposition") != NULL);
    
    if (is_multipart) {
        // multipart/form-data: Suche nach name="key" oder name='key'
        char name_pattern1[128];
        char name_pattern2[128];
        snprintf(name_pattern1, sizeof(name_pattern1), "name=\"%s\"", key);
        snprintf(name_pattern2, sizeof(name_pattern2), "name='%s'", key);
        
        char* name_pos = strstr(post_data, name_pattern1);
        if (!name_pos) {
            name_pos = strstr(post_data, name_pattern2);
        }
        
        if (name_pos) {
            // Suche nach Leerzeile nach dem Header (Start der Daten)
            char* data_start = strstr(name_pos, "\n\n");
            if (!data_start) {
                data_start = strstr(name_pos, "\r\n\r\n");
            }
            
            if (data_start) {
                // Skip "\n\n" oder "\r\n\r\n" - aber prüfe, ob es wirklich 2 oder 4 Zeichen sind
                if (data_start[0] == '\r' && data_start[1] == '\n' && data_start[2] == '\r' && data_start[3] == '\n') {
                    data_start += 4;  // Skip "\r\n\r\n"
                } else if (data_start[0] == '\n' && data_start[1] == '\n') {
                    data_start += 2;  // Skip "\n\n"
                } else {
                    // Fallback: Skip bis zum ersten Zeichen nach Leerzeile
                    while (*data_start == '\r' || *data_start == '\n') {
                        data_start++;
                    }
                }
                
                // Suche nach nächstem Boundary (beginnt mit "------" oder "----")
                // Boundary kann am Anfang der Zeile stehen (nach \n oder \r\n) oder am Ende der Datei
                char* data_end = strstr(data_start, "\r\n------");
                if (!data_end) {
                    data_end = strstr(data_start, "\n------");
                }
                if (!data_end) {
                    data_end = strstr(data_start, "\r\n----");
                }
                if (!data_end) {
                    data_end = strstr(data_start, "\n----");
                }
                // Prüfe auch, ob das Boundary direkt am Anfang steht (für letztes Part)
                if (!data_end && strncmp(data_start, "------", 6) == 0) {
                    data_end = data_start;
                }
                if (!data_end && strncmp(data_start, "----", 4) == 0) {
                    data_end = data_start;
                }
                if (!data_end) {
                    data_end = (char*)post_data + post_len;
                }
                
                size_t data_len = data_end - data_start;
                // Entferne trailing \r\n
                while (data_len > 0 && (data_start[data_len - 1] == '\n' || data_start[data_len - 1] == '\r')) {
                    data_len--;
                }
                
                if (data_len > 0 && data_len < max_len) {
                    strncpy(value, data_start, data_len);
                    value[data_len] = '\0';
                    return true;
                }
            }
        }
    } else {
        // application/x-www-form-urlencoded: Suche nach key=
        char key_with_eq[128];
        snprintf(key_with_eq, sizeof(key_with_eq), "%s=", key);
        char* param_start = strstr(post_data, key_with_eq);
        
        if (param_start) {
            param_start += strlen(key_with_eq);
            char* param_end = strchr(param_start, '&');
            size_t param_len = param_end ? (param_end - param_start) : strlen(param_start);
            
            if (param_len > 0 && param_len < max_len) {
                strncpy(value, param_start, param_len);
                value[param_len] = '\0';
                url_decode(value);
                return true;
            }
        }
    }
    
    return false;
}

// Handler für /config/save (POST)
static esp_err_t config_save_handler(httpd_req_t *req) {
    last_web_activity_us = esp_timer_get_time();
    
    // Content-Length prüfen und Puffer dynamisch allokieren
    const size_t MAX_POST_SIZE = 8192;  // Max. 8KB für POST-Daten (Sicherheitsgrenze)
    const size_t STATIC_BUFFER_SIZE = 4096;  // Statischer Puffer für normale Fälle
    
    size_t content_len = req->content_len;
    
    // Validierung: Content-Length muss vorhanden und innerhalb der Grenzen sein
    if (content_len == 0 || content_len > MAX_POST_SIZE) {
        ESP_LOGE(TAG, "POST-Daten ungültig: Content-Length=%zu (Max: %zu)", content_len, MAX_POST_SIZE);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Fehler: POST-Daten zu groß (%zu Bytes, Max: %zu Bytes)", 
                 content_len, MAX_POST_SIZE);
        httpd_resp_send(req, error_msg, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // Puffer allokieren (statisch oder dynamisch)
    char* post_data = NULL;
    bool use_dynamic = (content_len >= STATIC_BUFFER_SIZE);
    
    if (use_dynamic) {
        // Dynamische Allokation für größere Daten
        post_data = (char*)malloc(content_len + 1);  // +1 für Null-Terminator
        if (!post_data) {
            ESP_LOGE(TAG, "Speicher-Allokation fehlgeschlagen für %zu Bytes", content_len);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "Fehler: Speicher-Allokation fehlgeschlagen", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        ESP_LOGD(TAG, "Dynamischer Puffer allokiert: %zu Bytes", content_len);
    } else {
        // Statischer Puffer für normale Fälle
        static char static_post_data[STATIC_BUFFER_SIZE];
        post_data = static_post_data;
        ESP_LOGD(TAG, "Statischer Puffer verwendet: %zu Bytes", STATIC_BUFFER_SIZE);
    }
    
    // POST-Daten lesen
    size_t buffer_size = use_dynamic ? (content_len + 1) : STATIC_BUFFER_SIZE;
    int len = read_post_data(req, post_data, buffer_size);
    if (len < 0) {
        ESP_LOGE(TAG, "Fehler beim Lesen der POST-Daten");
        if (use_dynamic) {
            free(post_data);
        }
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: POST-Daten konnten nicht gelesen werden", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // Parameter extrahieren (wie Arduino's hasParam/getParam mit true)
    char json_data[1024] = "";
    char current_password[64] = "";
    
    // Prüfe ob "data" Parameter vorhanden (wie Arduino's hasParam("data", true))
    if (!get_post_param(post_data, len, "data", json_data, sizeof(json_data))) {
        ESP_LOGE(TAG, "FEHLER: Parameter 'data' fehlt");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: Parameter 'data' fehlt", HTTPD_RESP_USE_STRLEN);
        // Puffer freigeben (falls dynamisch allokiert)
        if (use_dynamic) {
            free(post_data);
        }
        return ESP_OK;
    }
    
    // current_password Parameter (optional, wie Arduino's hasParam("current_password", true))
    get_post_param(post_data, len, "current_password", current_password, sizeof(current_password));
    
    // Authentifizierung prüfen
    bool auth_ok = false;
    if (check_basic_auth(req, "admin", config_rtc.adminpass)) {
        auth_ok = true;
    } else if (strlen(current_password) > 0 && strcmp(current_password, config_rtc.adminpass) == 0) {
        auth_ok = true;
    }
    
    if (!auth_ok) {
        // WICHTIG: WWW-Authenticate Header VOR Status setzen (Browser-Kompatibilität)
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"GasOMeterKonfiguration\"");
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, NULL, 0);
        // Puffer freigeben (falls dynamisch allokiert)
        if (use_dynamic) {
            free(post_data);
        }
        return ESP_FAIL;
    }
    
    // DEBUG: Zeige empfangene JSON-Daten (wie Arduino)
    ESP_LOGI(TAG, "=== Empfangene Config-Daten ===");
    ESP_LOGI(TAG, "%s", json_data);
    ESP_LOGI(TAG, "===============================");
    
    // JSON parsen
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json_data);
    if (error) {
        ESP_LOGE(TAG, "JSON Parse Fehler: %s", error.c_str());
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: JSON ungültig", HTTPD_RESP_USE_STRLEN);
        // Puffer freigeben (falls dynamisch allokiert)
        if (use_dynamic) {
            free(post_data);
        }
        return ESP_OK;
    }
    
    // DEBUG: Zeige geparste Werte (wie Arduino)
    ESP_LOGI(TAG, "=== Geparste Config-Werte ===");
    if (doc["hostname"].is<const char*>()) {
        ESP_LOGI(TAG, "  hostname: %s", doc["hostname"].as<const char*>());
    }
    if (doc["adminpass"].is<const char*>()) {
        const char* adminpass = doc["adminpass"].as<const char*>();
        ESP_LOGI(TAG, "  adminpass: %s (Länge: %zu)", adminpass, strlen(adminpass));
    }
    if (doc["wakeup_minutes"].is<uint8_t>()) {
        ESP_LOGI(TAG, "  wakeup_minutes: %d", doc["wakeup_minutes"].as<uint8_t>());
    }
    if (doc["transfer_minutes"].is<uint8_t>()) {
        ESP_LOGI(TAG, "  transfer_minutes: %d", doc["transfer_minutes"].as<uint8_t>());
    }
    if (doc["adc_voltage_offset"].is<float>()) {
        ESP_LOGI(TAG, "  adc_voltage_offset: %.3f V", doc["adc_voltage_offset"].as<float>());
    }
    if (doc["ntp_server"].is<const char*>()) {
        ESP_LOGI(TAG, "  ntp_server: %s", doc["ntp_server"].as<const char*>());
    }
    if (doc["wifiCredentials"].is<JsonArray>()) {
        JsonArray credentials = doc["wifiCredentials"].as<JsonArray>();
        ESP_LOGI(TAG, "  wifiCredentials: %zu Set(s)", credentials.size());
        for (size_t i = 0; i < credentials.size() && i < 2; i++) {
            if (credentials[i]["ssid"].is<const char*>()) {
                ESP_LOGI(TAG, "    [%zu] SSID: %s", i, credentials[i]["ssid"].as<const char*>());
            }
            if (credentials[i]["password"].is<const char*>()) {
                const char* pwd = credentials[i]["password"].as<const char*>();
                ESP_LOGI(TAG, "    [%zu] Password: %s (Länge: %zu)", i, 
                         (strlen(pwd) > 0 ? "***" : "(leer)"), strlen(pwd));
            }
        }
    }
    ESP_LOGI(TAG, "==============================");
    
    // Config speichern
    bool wifi_credentials_changed = false;
    char errorMessage[256] = "";
    if (save_config(doc, &wifi_credentials_changed, errorMessage)) {
        config_rtc.config_loaded = false;
        
        // JSON-Antwort senden
        JsonDocument responseDoc;
        responseDoc["success"] = true;
        responseDoc["message"] = "Konfiguration erfolgreich gespeichert";
        responseDoc["wifi_changed"] = wifi_credentials_changed;
        
        char response_json[256];
        serializeJson(responseDoc, response_json, sizeof(response_json));
        
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, response_json, HTTPD_RESP_USE_STRLEN);
        
        // DEBUG: Erfolgsmeldung (wie Arduino)
        ESP_LOGI(TAG, "Config erfolgreich gespeichert (config.json)");
        ESP_LOGI(TAG, "  → RTC-Config invalidiert - wird beim nächsten Start aus config.json geladen");
        if (wifi_credentials_changed) {
            ESP_LOGI(TAG, "  → WiFi-Credentials wurden geändert");
        }
        
        // Puffer freigeben (falls dynamisch allokiert)
        if (use_dynamic) {
            free(post_data);
        }
        return ESP_OK;
    } else {
        // Fehler beim Speichern
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        if (strlen(errorMessage) > 0) {
            httpd_resp_send(req, errorMessage, HTTPD_RESP_USE_STRLEN);
        } else {
            httpd_resp_send(req, "Fehler: Ungültige Konfiguration", HTTPD_RESP_USE_STRLEN);
        }
        // Puffer freigeben (falls dynamisch allokiert)
        if (use_dynamic) {
            free(post_data);
        }
        return ESP_OK;
    }
}

// Handler für /reboot (POST)
// WICHTIG: Nur POST-Body wird akzeptiert, KEINE Query-Parameter (Sicherheitsmaßnahme gegen versehentliches Reset)
static esp_err_t reboot_handler(httpd_req_t *req) {
    // POST-Daten lesen (wie Arduino's hasParam("cmd", true))
    const size_t MAX_POST_SIZE = 512;  // Erhöht für multipart/form-data
    size_t content_len = req->content_len;
    
    // DEBUG: Zeige Content-Length
    ESP_LOGD(TAG, "Reboot-Handler: Content-Length=%zu", content_len);
    
    if (content_len == 0 || content_len > MAX_POST_SIZE) {
        ESP_LOGE(TAG, "Reboot-Handler: POST-Daten ungültig (Content-Length=%zu, Max=%zu)", content_len, MAX_POST_SIZE);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: POST-Daten ungültig", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    char post_data[512];
    int len = read_post_data(req, post_data, sizeof(post_data));
    if (len < 0) {
        ESP_LOGE(TAG, "Reboot-Handler: POST-Daten konnten nicht gelesen werden");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: POST-Daten konnten nicht gelesen werden", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // DEBUG: Zeige empfangene POST-Daten
    ESP_LOGD(TAG, "Reboot-Handler: POST-Daten empfangen (%d Bytes): %.200s", len, post_data);
    
    // POST-Parameter prüfen: cmd=reboot (wie Arduino's hasParam("cmd", true) && getParam("cmd", true)->value() == "reboot")
    char cmd[32] = "";
    if (!get_post_param(post_data, len, "cmd", cmd, sizeof(cmd))) {
        ESP_LOGE(TAG, "Reboot-Handler: Parameter 'cmd' nicht gefunden in POST-Daten");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: Parameter 'cmd=reboot' erforderlich", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    ESP_LOGD(TAG, "Reboot-Handler: cmd='%s'", cmd);
    
    if (strcmp(cmd, "reboot") != 0) {
        ESP_LOGE(TAG, "Reboot-Handler: cmd='%s' != 'reboot'", cmd);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: Parameter 'cmd=reboot' erforderlich", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // WICHTIG: Aktivität NACH Validierung aktualisieren, um Race-Condition mit Web-Timeout-Task zu vermeiden
    // Der Timeout-Task läuft asynchron und könnte zwischenzeitlich Deep-Sleep auslösen
    last_web_activity_us = esp_timer_get_time();
    
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Reboot wird durchgeführt...", HTTPD_RESP_USE_STRLEN);
    
    // Reboot-Variable setzen (wie Arduino: Variable setzen, Reboot im Task)
    reboot_requested = true;
    reboot_reason = "Reboot durch Web-Interface ausgelöst";
    
    return ESP_OK;
}

// Handler für /counter/set (POST)
static esp_err_t counter_set_handler(httpd_req_t *req) {
    last_web_activity_us = esp_timer_get_time();
    
    // POST-Daten lesen (wie Arduino's hasParam("value", true))
    const size_t MAX_POST_SIZE = 256;
    size_t content_len = req->content_len;
    if (content_len == 0 || content_len > MAX_POST_SIZE) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: POST-Daten ungültig", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    char post_data[256];
    int len = read_post_data(req, post_data, sizeof(post_data));
    if (len < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: POST-Daten konnten nicht gelesen werden", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // POST-Parameter prüfen: value (wie Arduino's hasParam("value", true))
    char value_str[32] = "";
    if (!get_post_param(post_data, len, "value", value_str, sizeof(value_str))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: Parameter 'value' fehlt", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    uint32_t new_value = (uint32_t)atoi(value_str);
    
    // Validierung: Maximal 9999999 (99999.99)
    if (new_value > 9999999) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: Wert zu groß (max. 99999.99)", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    uint32_t old_value = pulse_counter;
    ESP_LOGI(TAG, "Zählerstand manuell gesetzt: %lu → %lu", old_value, new_value);
    
    // Alten Wert in Ring-Speicher schreiben (falls > 0 und > max_pulse)
    if (old_value > 0) {
        uint32_t max_pulse = 0;
        uint32_t max_index = 0;
        max_pulse = find_max_pulse_and_index_from_nvs(&max_index);
        
        if (old_value > max_pulse) {
            ESP_LOGI(TAG, "Alter Wert (%lu) > max_pulse (%lu) → schreibe in Ring-Speicher", old_value, max_pulse);
            write_pulse_counter_to_ring_buffer();
        }
    }
    
    // Neuen Wert in RTC-RAM setzen
    pulse_counter = new_value;
    
    // Wenn neuer Wert < alter Wert: Stelle sicher, dass neuer Wert der höchste im Ring-Speicher ist
    if (new_value < old_value) {
        ESP_LOGI(TAG, "Neuer Wert (%lu) < alter Wert (%lu) → lösche alle Werte > %lu im Ring-Speicher", 
                 new_value, old_value, new_value);
        
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open_from_partition(NVS_PARTITION_PULSE, NVS_NAMESPACE_PULSE, NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK) {
            uint32_t deleted_count = 0;
            for (uint32_t i = 0; i < RING_BUFFER_SIZE; i++) {
                char key[MAX_KEY_LENGTH];
                snprintf(key, sizeof(key), "%s%lu", NVS_KEY_PREFIX, i);
                
                uint32_t pulse_value = 0;
                err = nvs_get_u32(nvs_handle, key, &pulse_value);
                
                if (err == ESP_OK && pulse_value > new_value) {
                    nvs_erase_key(nvs_handle, key);
                    deleted_count++;
                }
            }
            nvs_close(nvs_handle);
            ESP_LOGI(TAG, "Ring-Speicher bereinigt: %lu Einträge gelöscht", deleted_count);
        }
    }
    
    // Neuen Wert in Ring-Speicher schreiben
    init_pulse_nvs_minimal();
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open_from_partition(NVS_PARTITION_PULSE, NVS_NAMESPACE_PULSE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        ring_idx = (ring_idx + 1) % RING_BUFFER_SIZE;
        char key[MAX_KEY_LENGTH];
        snprintf(key, sizeof(key), "%s%lu", NVS_KEY_PREFIX, ring_idx);
        nvs_set_u32(nvs_handle, key, new_value);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    
    // Erfolgreiche Antwort
    char response[100];
    snprintf(response, sizeof(response), "Zählerstand gesetzt: %05lu.%02lu", new_value / 100, new_value % 100);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler für /wifi/scan (GET)
static esp_err_t wifi_scan_handler(httpd_req_t *req) {
    // Basic Auth prüfen
    if (!check_basic_auth(req, "admin", config_rtc.adminpass)) {
        // WICHTIG: WWW-Authenticate Header VOR Status setzen (Browser-Kompatibilität)
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"GasOMeterKonfiguration\"");
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }
    
    last_web_activity_us = esp_timer_get_time();
    
    // WiFi-Scan durchführen
    if (!init_wifi()) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"WiFi-Scan fehlgeschlagen\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    esp_wifi_set_mode(WIFI_MODE_STA);
    
    // WiFi starten (erforderlich für Scan)
    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"WiFi-Start fehlgeschlagen\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    vTaskDelay(pdMS_TO_TICKS(100));  // Kurze Verzögerung für WiFi-Initialisierung
    
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
    };
    
    ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"WiFi-Scan fehlgeschlagen\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    wifi_ap_record_t ap_records[ap_count];
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    
    // Nach RSSI sortieren (Bubble Sort)
    for (int i = 0; i < ap_count - 1; i++) {
        for (int j = 0; j < ap_count - i - 1; j++) {
            if (ap_records[j].rssi < ap_records[j + 1].rssi) {
                wifi_ap_record_t temp = ap_records[j];
                ap_records[j] = ap_records[j + 1];
                ap_records[j + 1] = temp;
            }
        }
    }
    
    // JSON erstellen (max. 10 stärkste Netzwerke)
    JsonDocument doc;
    JsonArray networksArray = doc.to<JsonArray>();
    int maxNetworks = (ap_count > 10) ? 10 : ap_count;
    
    for (int i = 0; i < maxNetworks; i++) {
        JsonObject network = networksArray.add<JsonObject>();
        network["ssid"] = (const char*)ap_records[i].ssid;
        network["rssi"] = ap_records[i].rssi;
        network["encrypted"] = (ap_records[i].authmode != WIFI_AUTH_OPEN);
    }
    
    char json_response[1024];
    serializeJson(doc, json_response, sizeof(json_response));
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler für / (Root - Redirect zu /index.html)
static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/index.html");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Handler für 404 (Not Found)
static esp_err_t not_found_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Not Found", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler für Static Files
static esp_err_t static_file_handler(httpd_req_t *req) {
    last_web_activity_us = esp_timer_get_time();
    
    // Blockiere config.json
    if (strcmp(req->uri, "/config.json") == 0) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Forbidden: config.json is protected", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // Dateipfad konstruieren
    char filepath[64];
    if (strcmp(req->uri, "/") == 0) {
        strcpy(filepath, "/spiffs/index.html");
    } else {
        snprintf(filepath, sizeof(filepath), "/spiffs%s", req->uri);
    }
    
    FILE* file = fopen(filepath, "r");
    if (!file) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "File not found", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // Content-Type bestimmen
    if (strstr(req->uri, ".css")) {
        httpd_resp_set_type(req, "text/css");
    } else if (strstr(req->uri, ".js")) {
        httpd_resp_set_type(req, "application/javascript");
    } else if (strstr(req->uri, ".gz")) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        // Content-Type basierend auf Dateiname ohne .gz
        if (strstr(req->uri, ".css.gz")) {
            httpd_resp_set_type(req, "text/css");
        } else if (strstr(req->uri, ".js.gz")) {
            httpd_resp_set_type(req, "application/javascript");
        }
    } else if (strstr(req->uri, ".html")) {
        httpd_resp_set_type(req, "text/html");
    } else if (strstr(req->uri, ".png")) {
        httpd_resp_set_type(req, "image/png");
    } else if (strstr(req->uri, ".ico")) {
        httpd_resp_set_type(req, "image/x-icon");
    }
    
    // Cache-Header für bootstrap.min.css
    if (strstr(req->uri, "bootstrap.min.css")) {
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");
    }
    
    // Datei senden
    char buffer[512];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, bytes_read) != ESP_OK) {
            fclose(file);
            return ESP_FAIL;
        }
    }
    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// Handler für /bootstrap.min.css (gzip)
static esp_err_t bootstrap_css_handler(httpd_req_t *req) {
    last_web_activity_us = esp_timer_get_time();
    
    FILE* file = fopen("/spiffs/bootstrap.min.css.gz", "r");
    if (!file) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "File not found", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");
    
    char buffer[512];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, bytes_read) != ESP_OK) {
            fclose(file);
            return ESP_FAIL;
        }
    }
    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

void setupWebServer() {
    // WICHTIG: LittleFS muss gemountet sein, bevor Web-Server startet
    // (auch wenn Config bereits aus RTC-RAM geladen wurde)
    if (!mount_littlefs()) {
        ESP_LOGE(TAG, "FEHLER: LittleFS konnte nicht gemountet werden - Web-Server kann nicht starten!");
        return;
    }
    
    // Server bereits gestartet?
    if (server != NULL) {
        ESP_LOGW(TAG, "Web-Server läuft bereits");
        return;
    }
    
    // HTTP-Server konfigurieren
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.max_resp_headers = 8;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;  // Wildcard-Matching für statische Dateien aktivieren
    config.stack_size = 12288;  // Stack-Größe: 12 KB (für große POST-Requests und verschachtelte Funktionsaufrufe)
                                  // Worst Case: config_save_handler (~3.5 KB) + save_config (~2.8 KB) + Overhead (~2 KB) = ~8.3 KB
                                  // Reserve: ~3.7 KB für größere JSON-Dokumente und zusätzliche Funktionsaufrufe
    
    // Server starten
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Web-Server konnte nicht gestartet werden");
        return;
    }
    
    // URI-Handler registrieren
    httpd_uri_t root_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = root_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &root_uri);
    
    httpd_uri_t index_uri = {
        .uri       = "/index.html",
        .method    = HTTP_GET,
        .handler   = index_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &index_uri);
    
    // Handler für /index.html (HEAD) - für Browser-Prüfungen
    httpd_uri_t index_head_uri = {
        .uri       = "/index.html",
        .method    = HTTP_HEAD,
        .handler   = index_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &index_head_uri);
    
    httpd_uri_t config_uri = {
        .uri       = "/config",
        .method    = HTTP_GET,
        .handler   = config_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &config_uri);
    
    httpd_uri_t bootstrap_uri = {
        .uri       = "/bootstrap.min.css",
        .method    = HTTP_GET,
        .handler   = bootstrap_css_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &bootstrap_uri);
    
    httpd_uri_t ping_uri = {
        .uri       = "/ping",
        .method    = HTTP_GET,
        .handler   = ping_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &ping_uri);
    
    httpd_uri_t version_uri = {
        .uri       = "/version",
        .method    = HTTP_GET,
        .handler   = version_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &version_uri);
    
    httpd_uri_t reading_uri = {
        .uri       = "/reading",
        .method    = HTTP_GET,
        .handler   = reading_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &reading_uri);
    
    // POST-Handler registrieren
    httpd_uri_t config_save_uri = {
        .uri       = "/config/save",
        .method    = HTTP_POST,
        .handler   = config_save_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &config_save_uri);
    
    httpd_uri_t reboot_uri = {
        .uri       = "/reboot",
        .method    = HTTP_POST,
        .handler   = reboot_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &reboot_uri);
    
    httpd_uri_t counter_set_uri = {
        .uri       = "/counter/set",
        .method    = HTTP_POST,
        .handler   = counter_set_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &counter_set_uri);
    
    httpd_uri_t wifi_scan_uri = {
        .uri       = "/wifi/scan",
        .method    = HTTP_GET,
        .handler   = wifi_scan_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &wifi_scan_uri);
    
    // Wildcard-Handler für alle statischen Dateien (muss als letzter registriert werden)
    // Spezifische Handler (oben) haben Vorrang vor dem Wildcard-Handler
    httpd_uri_t static_uri = {
        .uri       = "/*",
        .method    = HTTP_GET,
        .handler   = static_file_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &static_uri);
    
    server_started = true;
    last_web_activity_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Web-Server gestartet");
}

// ============================================
// Entry Point (ESP-IDF)
// ============================================
extern "C" void app_main(void) {
    // Logging initialisieren (automatisch in ESP-IDF)
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    
    // Event-System initialisieren (für WiFi, etc.)
    esp_netif_init();
    esp_event_loop_create_default();
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Antennenumschaltung initialisieren (interne Antenne als Standard)
    INIT_ANTENNA_SWITCH(ANTENNA_INTERNAL);
    
    // Taster A (BUTTON_A_GPIO) für Wake-up konfigurieren (INPUT_PULLUP, active-low)
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << BUTTON_A_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    
    // Power-On vs. Wake-up erkennen
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    isPowerOn = (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED);
    
    // Wake-up Count nur bei Deep-Sleep-Wake-up erhöhen (nicht bei ESP.restart())
    if (!isPowerOn) {
        ++wakeupCount;
    }
    
    // Wake-up-Informationen ausgeben (kombiniert)
    ESP_LOGI(TAG, "\n=== Gas-O-Meter ===");
    ESP_LOGI(TAG, "Wake-up Count: %d", wakeupCount);
    
    switch (wakeup_reason) {
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            ESP_LOGI(TAG, "=== EVENT: Power-On ===");
            break;
        case ESP_SLEEP_WAKEUP_GPIO:
            ESP_LOGI(TAG, "=== EVENT: Wake-up durch GPIO (Taster A) ===");
            break;
        case ESP_SLEEP_WAKEUP_TIMER:
            ESP_LOGI(TAG, "=== EVENT: Wake-up durch Timer (Cron-Intervall) ===");
            break;
        case ESP_SLEEP_WAKEUP_EXT0:
        case ESP_SLEEP_WAKEUP_EXT1:
            ESP_LOGI(TAG, "=== EVENT: Wake-up durch GPIO (EXT) ===");
            break;
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
            ESP_LOGI(TAG, "=== EVENT: Wake-up durch Touchpad ===");
            break;
        case ESP_SLEEP_WAKEUP_ULP:
            ESP_LOGI(TAG, "=== EVENT: Wake-up durch ULP ===");
            break;
        default:
            ESP_LOGI(TAG, "=== EVENT: Wake-up durch Unbekannt (0x%x) ===", wakeup_reason);
            break;
    }
    
    // Interne LED initialisieren und sofort einschalten (HP-Core läuft)
    // WICHTIG: LED wird hier eingeschaltet, um zu zeigen, dass HP-Core aktiv ist
    io_conf.pin_bit_mask = (1ULL << LED_BUILTIN_GPIO);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)LED_BUILTIN_GPIO, LED_ON);  // LED EIN (HP-Core aktiv)
    ESP_LOGI(TAG, "LED initialisiert (GPIO%d)", LED_BUILTIN_GPIO);
    
    // ADC initialisieren (ESP-IDF)
    // WICHTIG: Spannungsprüfung VOR NVS-Initialisierung, um Akku zu schützen!
    esp_err_t adc_ret = init_adc();
    if (adc_ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC Fehler: %s", esp_err_to_name(adc_ret));
    } else {
        // Akku-Messung
        battery_adc_mv = read_adc_median_mv();
        battery_voltage = (float)battery_adc_mv / 1000.0f * VOLTAGE_DIVIDER_RATIO + config_rtc.adc_voltage_offset;
        battery_percent = VOLTAGE_TO_PERCENT(battery_voltage);
        
        ESP_LOGI(TAG, "ADC: %d mV, Spannung: %.2f V, Prozent: %d%%", 
                 battery_adc_mv, battery_voltage, battery_percent);
        
        // Stromversorgungs-Prüfung ZUERST (vor Akku-Schutz)
        // WICHTIG: Bei USB-Stromversorgung kann Timer aktiv bleiben (keine Akku-Probleme)
        bool is_usb_power = (battery_voltage < USB_DETECTION_THRESHOLD);
        bool enable_timer_wakeup = true;  // Standard: Timer aktiviert
        
        if (is_usb_power) {
            // USB-Stromversorgung: Timer kann aktiv bleiben (keine Akku-Probleme)
            ESP_LOGI(TAG, "USB-Stromversorgung erkannt - Betrieb fortgesetzt");
            ESP_LOGI(TAG, "Spannung: %.2f V (USB-Schwelle: %.2f V)", 
                     battery_voltage, USB_DETECTION_THRESHOLD);
            // enable_timer_wakeup bleibt true
        } else {
            // Akku-Betrieb: Timer-Wake-up deaktivieren bei kritischer Spannung
            // WICHTIG: Bei Spannung <= BATTERY_VOLTAGE_PROTECTION macht automatisches Wake-up nur mehr Schaden
            // Nur noch manueller Wake-up über Taster möglich
            if (battery_voltage <= BATTERY_VOLTAGE_PROTECTION) {
                enable_timer_wakeup = false;
                ESP_LOGW(TAG, "Akku-Spannung kritisch (<= BATTERY_VOLTAGE_PROTECTION)");
                ESP_LOGW(TAG, "Timer-Wake-up wird DEAKTIVIERT - nur manueller Wake-up über Taster A möglich");
                ESP_LOGW(TAG, "Spannung: %.2f V (Schutz-Schwelle: %.2f V)", 
                         battery_voltage, BATTERY_VOLTAGE_PROTECTION);
            }
            
            // Akku-Schutz: Deep-Sleep bei zu niedriger Spannung
            if (battery_voltage < BATTERY_VOLTAGE_20) {
            // Spannung >= 2V aber < BATTERY_VOLTAGE_20: Akku zu niedrig → Deep-Sleep zum Schutz
            ESP_LOGW(TAG, "Akku zu niedrig - Deep-Sleep zum Akku-Schutz");
            ESP_LOGW(TAG, "Spannung: %.2f V (Minimum: %.2f V)", 
                     battery_voltage, BATTERY_VOLTAGE_20);
            
                // Vor Deep-Sleep: pulse_counter prüfen und ggf. in Ring-Speicher schreiben
            // (bei Akku-Low kann RTC-RAM verloren gehen)
            ESP_LOGI(TAG, "Speichere pulse_counter in Ring-Speicher vor Deep-Sleep (Akku-Low)...");
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
            ESP_LOGI(TAG, "Prüfe NVS-Ring-Speicher-Version...");
            ESP_LOGI(TAG, "Erwartete Version (Build-Timestamp): %lu", RING_BUFFER_VERSION);
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
            ESP_LOGI(TAG, "LP-Core Watchdog Task gestartet");
            
        // ring_idx und pulse_counter initialisieren (kombiniert)
        // ring_idx: Ring-Buffer-Index (aus RTC-RAM oder Ring-Speicher)
        // pulse_counter: Puls-Zähler (aus RTC-RAM oder Ring-Speicher)
        init_ring_buffer_and_pulse_counter(isPowerOn);
        
        // 1. Batteriespannung-Test und ggf. in Ring-Speicher schreiben
        // < 30% ODER USB: Schreibe in Ring-Speicher (RTC-RAM könnte verloren gehen)
        // >= 30%: Kein Schreiben (RTC-RAM bleibt erhalten)
        if (battery_voltage < BATTERY_VOLTAGE_30 || battery_voltage < USB_DETECTION_THRESHOLD) {
            ESP_LOGI(TAG, "Speichere pulse_counter in Ring-Speicher (< 30%% oder USB)...");
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
                time_t now;
                time(&now);
                if (localtime_r(&now, &timeinfo) && (timeinfo.tm_min % config_rtc.transfer_minutes == 0) && config_available) {
                    ESP_LOGI(TAG, "=== Timer-Wake-up: Datenübertragung (Minute %d, Intervall: %d Min) ===",
                             timeinfo.tm_min, config_rtc.transfer_minutes);
                    ESP_LOGI(TAG, "Hier sollten jetzt die Daten übertragen werden....................");
                    should_enter_deep_sleep = true;
                    deep_sleep_reason = "Timer-Wake-up: Datenübertragung abgeschlossen";
                } else {
                    uint8_t current_min = localtime_r(&now, &timeinfo) ? timeinfo.tm_min : 0;
                    ESP_LOGI(TAG, "=== Timer-Wake-up: Keine Übertragung (Minute %d, Intervall: %d Min) ===",
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
            ESP_LOGI(TAG, "Config erfolgreich geladen");
            
            // WiFi verbinden
            if (connect_wifi()) {
                        // mDNS starten (für .local Domain)
                        esp_err_t mdns_ret = mdns_init();
                        if (mdns_ret == ESP_OK) {
                            mdns_hostname_set(config_rtc.hostname);
                            mdns_instance_name_set("Gas-O-Meter");
                            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
                            ESP_LOGI(TAG, "mDNS: http://%s.local", config_rtc.hostname);
                        } else {
                            ESP_LOGE(TAG, "mDNS-Initialisierung fehlgeschlagen: %s", esp_err_to_name(mdns_ret));
                        }
                        
                // NTP-Zeitsynchronisation
                sync_ntp_time();
                
                // Web-Server starten
                setupWebServer();
                        char ip_str[16];
                        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&wifi_ip_info.ip));
                        ESP_LOGI(TAG, "Web-Server: http://%s", ip_str);
                    } else {
                        ESP_LOGE(TAG, "WiFi-Verbindung fehlgeschlagen → Starte Access Point");
                        
                        // Access Point starten
                        if (start_access_point()) {
                            // Web-Server starten (ohne mDNS und NTP im AP-Modus)
                            setupWebServer();
                            ESP_LOGI(TAG, "Web-Server (AP-Modus): http://%d.%d.%d.%d", 
                                     AP_IP_ADDRESS_1, AP_IP_ADDRESS_2, AP_IP_ADDRESS_3, AP_IP_ADDRESS_4);
            } else {
                            ESP_LOGE(TAG, "FEHLER: Weder WiFi-Verbindung noch Access Point möglich");
                        }
            }
        } else {
                    ESP_LOGE(TAG, "Config-Laden fehlgeschlagen → WiFi/Web-Server nicht gestartet");
        }
                break;
    }
    }  // Ende des else-Blocks (ADC erfolgreich)
    
    // Web-Timeout Task starten (ersetzt Arduino loop())
    // Funktionsdeklaration: void web_timeout_task(void *parameter);
    xTaskCreate(
        web_timeout_task,      // Task-Funktion
        "web_timeout",         // Task-Name
        4096,                  // Stack-Größe (Bytes)
        NULL,                  // Parameter
        1,                     // Priorität
        NULL                   // Task-Handle (nicht benötigt)
    );
    // Log-Meldung erfolgt in web_timeout_task() selbst, wenn der Task tatsächlich startet
}

// ============================================
// FreeRTOS Task: Web-Server Timeout und Deep-Sleep Management
// ============================================
// Ersetzt die Arduino loop() Funktion
void web_timeout_task(void *parameter) {
    const TickType_t check_interval = pdMS_TO_TICKS(1000);  // 1 Sekunde
    const uint64_t sleep_threshold_us = WIFI_WAIT_FOR_SLEEP * 60 * 1000000ULL;  // Minuten in Mikrosekunden
    const uint64_t recent_activity_threshold_us = 30 * 1000000ULL;  // 30 Sekunden in Mikrosekunden
    
    ESP_LOGI(TAG, "Web-Timeout Task gestartet");
    
    while (1) {
        // Reboot-Prüfung (wie Arduino: Variable prüfen und Reboot durchführen)
        if (reboot_requested) {
            ESP_LOGI(TAG, "Reboot angefordert: %s", reboot_reason ? reboot_reason : "Unbekannt");
            vTaskDelay(pdMS_TO_TICKS(500));  // Warte auf Antwort-Übertragung
            perform_reboot(reboot_reason ? reboot_reason : "Reboot angefordert");
            // Sollte nie hier ankommen
        }
        
        // Web-Timeout-Prüfung
        if (server_started) {
            uint64_t current_time_us = esp_timer_get_time();
            uint64_t inactivity_us = current_time_us - last_web_activity_us;
            bool recent_activity = (inactivity_us < recent_activity_threshold_us);
            
            if (inactivity_us >= sleep_threshold_us && !should_enter_deep_sleep && !recent_activity) {
                ESP_LOGI(TAG, "Keine Web-Server-Aktivität seit %d Minuten → Deep-Sleep", WIFI_WAIT_FOR_SLEEP);
                should_enter_deep_sleep = true;
                deep_sleep_reason = "Web-Server-Inaktivität";
            } else if (recent_activity && inactivity_us >= sleep_threshold_us) {
                // Aktivität war sehr aktuell, aber Timeout erreicht → warte noch etwas
                ESP_LOGI(TAG, "Aktive Verbindung erkannt (letzte Aktivität vor %llu ms) → Deep-Sleep verzögert", 
                         inactivity_us / 1000);
            }
        }
        
        // Deep-Sleep-Prüfung (wie in loop())
        if (should_enter_deep_sleep) {
            ESP_LOGI(TAG, "%s", deep_sleep_reason);
            
            // Ring-Speicher-Prüfung (einmalig, zentralisiert)
            // < 30% ODER USB: Schreibe in Ring-Speicher (RTC-RAM könnte verloren gehen)
            // >= 30%: Kein Schreiben (RTC-RAM bleibt erhalten)
            if (battery_voltage < BATTERY_VOLTAGE_30 || battery_voltage < USB_DETECTION_THRESHOLD) {
                ESP_LOGI(TAG, "Speichere pulse_counter in Ring-Speicher vor Deep-Sleep (< 30%% oder USB)...");
                write_pulse_counter_to_ring_buffer();
            } else {
                ESP_LOGI(TAG, "Akku-Spannung OK (>= 30%%) → pulse_counter bleibt im RTC-RAM (kein Schreiben nötig)");
            }
            
            // Timer-Wake-up: Bei USB-Stromversorgung immer aktivieren
            // Bei Akku-Betrieb: Nur aktivieren, wenn Spannung > BATTERY_VOLTAGE_PROTECTION
            bool is_usb_power = (battery_voltage < USB_DETECTION_THRESHOLD);
            bool enable_timer = is_usb_power || (battery_voltage > BATTERY_VOLTAGE_PROTECTION);
            enter_deep_sleep_with_gpio_and_timer_wakeup(enable_timer);
            
            // Wenn wir hier ankommen, wurde Deep-Sleep nicht gestartet (z.B. keine Wake-up-Quelle)
            // Flag zurücksetzen, um Endlosschleife zu vermeiden
            should_enter_deep_sleep = false;
            ESP_LOGW(TAG, "Deep-Sleep konnte nicht gestartet werden - System bleibt aktiv");
        }
        
        vTaskDelay(check_interval);
    }
}

