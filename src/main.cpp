#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WiFi.h>
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
// ESP-IDF LP-Core Header
// HINWEIS: Mit framework = arduino, espidf sind die ESP-IDF Header verfügbar
#include <esp_lp_core.h>
#include <esp_lp_core_bootloader.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ============================================
// RTC Memory Struktur für Config-Werte
// ============================================
typedef struct {
    char hostname[32];
    char adminpass[32];
    char wifi_ssid[32];
    char wifi_password[64];
    uint8_t wakeup_minutes;
    uint8_t transfer_minutes;
    float adc_voltage_offset;  // ADC-Offset-Korrektur in Volt (aus config.json oder hardware.h)
    char ntp_server[64];       // NTP-Server (aus config.json oder hardware.h)
    bool config_loaded;
} config_rtc_t;

RTC_DATA_ATTR config_rtc_t config_rtc = {
    .hostname = "gas-o-meter2",
    .adminpass = "",
    .wifi_ssid = "",
    .wifi_password = "",
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
bool spiffs_mounted = false;
bool server_started = false;  // Flag: Web-Server gestartet?

// Akku-Messwerte
uint32_t battery_adc_mv = 0;  // ADC-Wert in Millivolt (von analogReadMilliVolts - automatisch kalibriert)
float battery_voltage = 0.0f;
uint8_t battery_percent = 0;

// Web-Server Inaktivitäts-Timer
unsigned long last_web_activity = 0;  // Zeitpunkt der letzten Web-Server-Aktivität

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
// SPIFFS Mount (einmalig)
// ============================================
bool mount_spiffs() {
    if (spiffs_mounted) {
        return true;
    }
    
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS Mount fehlgeschlagen");
        return false;
    }
    
    spiffs_mounted = true;
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
// RTC pulse_counter: In Ring-Speicher schreiben (bei ESP.restart(), Akku-Low, USB)
// ============================================
bool write_pulse_counter_to_ring_buffer() {
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

// ============================================
// LP-Core Management
// ============================================
// HINWEIS: Die LP-Core API ist im Arduino Framework für ESP32C6 noch nicht verfügbar.
// Die gesamte Implementierung ist vollständig und wartet auf zukünftige Framework-Updates.
// Sobald die Header-Dateien (esp_lp_core.h, esp_lp_core_bootloader.h) verfügbar sind,
// wird der Code automatisch funktionieren.

// Forward-Declaration für LP-Core Hauptfunktion
extern "C" void lp_core_main(void);

bool start_lp_core() {
    // Prüfe ob LP-Core bereits läuft
    if (esp_lp_core_is_running()) {
        Serial.println("LP-Core läuft bereits");
        return true;
    }
    
    Serial.println("Starte LP-Core...");
    
    // LP-Core Bootloader initialisieren
    esp_lp_core_bootloader_config_t config = {
        .entry_addr = (uint32_t)lp_core_main,
    };
    
    esp_err_t err = esp_lp_core_bootloader_init(&config);
    if (err != ESP_OK) {
        Serial.printf("LP-Core Bootloader-Initialisierung fehlgeschlagen: %s\n", esp_err_to_name(err));
        return false;
    }
    
    // LP-Core starten
    err = esp_lp_core_start();
    if (err != ESP_OK) {
        Serial.printf("LP-Core Start fehlgeschlagen: %s\n", esp_err_to_name(err));
        return false;
    }
    
    // Kurz warten, damit LP-Core starten kann
    delay(100);
    
    // Prüfe ob LP-Core wirklich läuft
    if (esp_lp_core_is_running()) {
        Serial.println("LP-Core erfolgreich gestartet");
        // Watchdog-Zähler zurücksetzen (wird vom LP-Core erhöht)
        lp_core_running = 0;
        return true;
    } else {
        Serial.println("WARNUNG: LP-Core läuft nicht nach Start!");
        return false;
    }
}

// FreeRTOS Task für LP-Core Watchdog
void lp_core_watchdog_task(void *parameter) {
    uint32_t last_lp_core_value = 0;
    
    Serial.println("LP-Core Watchdog Task gestartet");
    
    // Prüfe ob LP-Core bereits läuft (nach Power-Up/ESP.restart() ist lp_core_running == 0)
    if (lp_core_running == 0) {
        // Nach Power-Up oder ESP.restart(): LP-Core direkt starten
        Serial.println("lp_core_running == 0 → Starte LP-Core...");
        if (start_lp_core()) {
            last_lp_core_value = lp_core_running;  // Neuer Wert nach Start
        } else {
            Serial.println("FEHLER: LP-Core Start fehlgeschlagen!");
            // Bei Fehler: Task beenden (wird bei nächstem Power-Up/ESP.restart() erneut gestartet)
            vTaskDelete(NULL);
            return;
        }
    } else {
        // LP-Core läuft bereits (z.B. nach Deep-Sleep-Wake-up)
        last_lp_core_value = lp_core_running;
        Serial.printf("LP-Core läuft bereits (Zähler: %lu)\n", last_lp_core_value);
    }
    
    // Watchdog-Schleife: Prüfe regelmäßig, ob LP-Core noch läuft
    while (1) {
        // Warte LP_CORE_WATCHDOG_MS bevor Prüfung
        vTaskDelay(pdMS_TO_TICKS(LP_CORE_WATCHDOG_MS));
        
        uint32_t current_lp_core_value = lp_core_running;
        
        // Prüfe ob Zähler sich erhöht hat
        if (current_lp_core_value == last_lp_core_value) {
            // Zähler hat sich nicht erhöht → LP-Core läuft nicht mehr!
            Serial.printf("WARNUNG: LP-Core Watchdog-Timeout! (Zähler: %lu, erwartet: > %lu)\n", 
                         current_lp_core_value, last_lp_core_value);
            Serial.println("Starte LP-Core neu...");
            
            // LP-Core neu starten
            if (start_lp_core()) {
                last_lp_core_value = lp_core_running;  // Neuer Wert nach Start
            } else {
                Serial.println("FEHLER: LP-Core Neustart fehlgeschlagen!");
                // Bei Fehler: Kurz warten und erneut versuchen
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        } else {
            // Zähler hat sich erhöht → LP-Core läuft
            Serial.printf("LP-Core Watchdog OK (Zähler: %lu → %lu)\n", 
                         last_lp_core_value, current_lp_core_value);
            last_lp_core_value = current_lp_core_value;
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
// Deep-Sleep mit GPIO-Wake-up konfigurieren
// ============================================
void enter_deep_sleep_with_gpio_wakeup() {
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
    
    if (is_rtc_gpio) {
        // RTC-GPIO verwenden (empfohlen für Deep-Sleep-Wake-up)
        Serial.printf("GPIO%d ist RTC-fähig → verwende RTC-GPIO-Funktionen\n", gpio_num);
        
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
        
    } else {
        // Normale GPIO-Funktionen verwenden (nicht RTC-fähig)
        Serial.printf("GPIO%d ist NICHT RTC-fähig → verwende normale GPIO-Funktionen\n", gpio_num);
        
        // Level basierend auf GPIO-Modus bestimmen
        if (gpio_mode == INPUT_PULLUP) {
            wakeup_level = GPIO_INTR_LOW_LEVEL;
            level_name = "LOW LEVEL (Taster gedrückt = LOW)";
            gpio_set_direction(gpio_num, GPIO_MODE_INPUT);
            gpio_set_pull_mode(gpio_num, GPIO_PULLUP_ONLY);
        } else if (gpio_mode == INPUT_PULLDOWN) {
            wakeup_level = GPIO_INTR_HIGH_LEVEL;
            level_name = "HIGH LEVEL (Taster gedrückt = HIGH)";
            gpio_set_direction(gpio_num, GPIO_MODE_INPUT);
            gpio_set_pull_mode(gpio_num, GPIO_PULLDOWN_ONLY);
        } else {
            wakeup_level = GPIO_INTR_LOW_LEVEL;
            level_name = "LOW LEVEL (Default)";
            gpio_set_direction(gpio_num, GPIO_MODE_INPUT);
            gpio_set_pull_mode(gpio_num, GPIO_FLOATING);
        }
        
        // Für nicht-RTC-GPIOs: HOLD manuell aktivieren
        // Laut Dokumentation wird HOLD automatisch verwendet, wenn RTC_PERIPH ausgeschaltet ist
        gpio_hold_en(gpio_num);
        Serial.println("GPIO-Hold manuell aktiviert (für nicht-RTC-GPIO)");
    }
    
    // WICHTIG: Kurze Verzögerung, damit Pull-Up/Pull-Down stabilisiert
    delay(50);
    
    // Aktuellen GPIO-Zustand prüfen (für Debugging und Warnung)
    int gpio_state = is_rtc_gpio ? rtc_gpio_get_level(gpio_num) : gpio_get_level(gpio_num);
    Serial.printf("BUTTON_A_GPIO aktueller Zustand: %s\n", gpio_state ? "HIGH" : "LOW");
    
    // WICHTIG: Bei Level-Mode weckt ESP32C6 sofort, wenn GPIO bereits im Wake-up-Level ist!
    // Daher prüfen und warnen, falls GPIO bereits im Wake-up-Level ist
    if ((wakeup_level == GPIO_INTR_LOW_LEVEL && gpio_state == 0) ||
        (wakeup_level == GPIO_INTR_HIGH_LEVEL && gpio_state == 1)) {
        Serial.println("WARNUNG: BUTTON_A_GPIO ist bereits im Wake-up-Level! System würde sofort wecken.");
        Serial.println("Stelle sicher, dass Taster nicht gedrückt ist, bevor Deep-Sleep startet!");
        if (!is_rtc_gpio) {
            gpio_hold_dis(gpio_num);  // HOLD deaktivieren, falls aktiviert
        }
        return;  // Deep-Sleep abbrechen
    }
    
    // GPIO-Wake-up aktivieren (ESP32C6 unterstützt nur gpio_wakeup, nicht ext0/ext1)
    esp_err_t ret = esp_sleep_enable_gpio_wakeup();
    if (ret != ESP_OK) {
        Serial.printf("FEHLER: GPIO-Wake-up-Konfiguration fehlgeschlagen: %s\n", esp_err_to_name(ret));
        Serial.println("Deep-Sleep wird ohne Wake-up-Konfiguration gestartet!");
        if (!is_rtc_gpio) {
            gpio_hold_dis(gpio_num);  // HOLD deaktivieren, falls aktiviert
        }
    } else {
        // GPIO als Wake-up-Source setzen: Level basierend auf GPIO-Modus
        // ESP32C6 unterstützt NUR Level-Mode (LOW_LEVEL oder HIGH_LEVEL)
        ret = gpio_wakeup_enable(gpio_num, wakeup_level);
        if (ret != ESP_OK) {
            Serial.printf("FEHLER: gpio_wakeup_enable fehlgeschlagen: %s\n", esp_err_to_name(ret));
            Serial.printf("Fehler-Code: %s\n", esp_err_to_name(ret));
            if (!is_rtc_gpio) {
                gpio_hold_dis(gpio_num);  // HOLD deaktivieren, falls aktiviert
            }
        } else {
            Serial.printf("Deep-Sleep konfiguriert: Wake-up bei Taster A (BUTTON_A_GPIO) via GPIO - %s\n", level_name);
            Serial.printf("GPIO-Modus: %s\n", 
                         gpio_mode == INPUT_PULLUP ? "INPUT_PULLUP" : 
                         (gpio_mode == INPUT_PULLDOWN ? "INPUT_PULLDOWN" : "INPUT"));
            Serial.println("HOLD-Funktion aktiv: Pull-Up/Pull-Down wird während Deep-Sleep gehalten");
        }
    }
    
    // LED ausschalten (HP-Core geht in Deep-Sleep)
    digitalWrite(LED_BUILTIN_GPIO, LED_OFF);
    Serial.println("Interne LED ausgeschaltet (Deep-Sleep)");
    
    // WICHTIG: Ressourcen ordnungsgemäß freigeben vor Deep-Sleep
    Serial.println("Schließe Ressourcen vor Deep-Sleep...");
    
    // 1. Web-Server stoppen
    if (server_started) {
        server.end();
        server_started = false;  // Flag zurücksetzen
        Serial.println("Web-Server gestoppt");
        delay(100);  // Kurze Verzögerung für sauberes Schließen
    }
    
    // 2. WiFi trennen und deaktivieren
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect(true);  // true = WiFi komplett deaktivieren
        Serial.println("WiFi getrennt und deaktiviert");
        delay(100);
    }
    WiFi.mode(WIFI_OFF);
    
    // 3. SPIFFS unmounten (sichert alle ausstehenden Schreibvorgänge)
    if (spiffs_mounted) {
        SPIFFS.end();
        spiffs_mounted = false;
        Serial.println("SPIFFS unmounted");
        delay(50);
    }
    
    Serial.println("Alle Ressourcen freigegeben");
    Serial.println("Gehe in Deep-Sleep...");
    Serial.flush();
    delay(200);  // Längere Pause, damit Serial-Output gesendet wird
    
    esp_deep_sleep_start();
    // Ab hier wird Code nicht mehr ausgeführt
}

// ============================================
// Config.json laden
// ============================================
bool load_config() {
    if (!mount_spiffs()) {
        return false;
    }
    
    File configFile = SPIFFS.open("/config.json", "r");
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
    DynamicJsonDocument doc(1024);
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
    if (doc.containsKey("wakeup_minutes")) {
        config_rtc.wakeup_minutes = doc["wakeup_minutes"].as<uint8_t>();
    } else {
        config_rtc.wakeup_minutes = DEFAULT_WAKEUP_INTERVAL_MIN;
    }
    
    // Transfer Intervall: aus config.json oder Default (DEFAULT_TRANSFER_INTERVAL_X * DEFAULT_WAKEUP_INTERVAL_MIN)
    if (doc.containsKey("tarnsfer_minutes")) {  // Tippfehler in JSON beibehalten
        config_rtc.transfer_minutes = doc["tarnsfer_minutes"].as<uint8_t>();
    } else {
        // Default: DEFAULT_TRANSFER_INTERVAL_X * DEFAULT_WAKEUP_INTERVAL_MIN
        config_rtc.transfer_minutes = DEFAULT_TRANSFER_INTERVAL_X * DEFAULT_WAKEUP_INTERVAL_MIN;
    }
    
    // ADC-Offset: aus config.json oder Default aus hardware.h
    if (doc.containsKey("adc_voltage_offset")) {
        config_rtc.adc_voltage_offset = doc["adc_voltage_offset"].as<float>();
    } else {
        config_rtc.adc_voltage_offset = ADC_VOLTAGE_OFFSET;
    }
    
    // NTP-Server: aus config.json oder Default aus hardware.h
    if (doc.containsKey("ntp_server")) {
        const char* ntp_server = doc["ntp_server"] | DEFAULT_NTP_SERVER;
        strncpy(config_rtc.ntp_server, ntp_server, sizeof(config_rtc.ntp_server) - 1);
        config_rtc.ntp_server[sizeof(config_rtc.ntp_server) - 1] = '\0';
    } else {
        strncpy(config_rtc.ntp_server, DEFAULT_NTP_SERVER, sizeof(config_rtc.ntp_server) - 1);
        config_rtc.ntp_server[sizeof(config_rtc.ntp_server) - 1] = '\0';
    }
    
    // WiFi-Credentials werden später beim Verbinden verwendet
    config_rtc.config_loaded = true;
    
    Serial.println("Config.json erfolgreich geladen");
    return true;
}

// ============================================
// WiFi-Verbindung mit höchster RSSI
// ============================================
bool connect_wifi() {
    if (!mount_spiffs()) {
        return false;
    }
    
    File configFile = SPIFFS.open("/config.json", "r");
    if (!configFile) {
        return false;
    }
    
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, configFile);
    configFile.close();
    
    if (error) {
        return false;
    }
    
    JsonArray credentials = doc["wifiCredentials"];
    if (credentials.size() == 0) {
        Serial.println("Keine WiFi-Credentials gefunden");
        return false;
    }
    
    // WiFi initialisieren (konsistent für Power-On und Reboot)
    // WICHTIG: WiFi-Modus explizit setzen, um sicherzustellen, dass WiFi bereit ist
    WiFi.mode(WIFI_OFF);  // Zuerst komplett ausschalten
    delay(100);
    WiFi.mode(WIFI_STA);  // Dann Station-Modus aktivieren
    WiFi.disconnect();    // Bestehende Verbindungen trennen
    delay(100);
    
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
    
    for (int i = 0; i < credentials.size(); i++) {
        const char* ssid = credentials[i]["ssid"];
        const char* password = credentials[i]["password"];
        
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
        strncpy(config_rtc.wifi_ssid, best_ssid, sizeof(config_rtc.wifi_ssid) - 1);
        config_rtc.wifi_ssid[sizeof(config_rtc.wifi_ssid) - 1] = '\0';
        strncpy(config_rtc.wifi_password, best_password, sizeof(config_rtc.wifi_password) - 1);
        config_rtc.wifi_password[sizeof(config_rtc.wifi_password) - 1] = '\0';
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
        return strlen(config_rtc.wifi_ssid) > 0 ? String(config_rtc.wifi_ssid) : String("Nicht verbunden");
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
        // Formatierung: 8 Stellen mit führenden Nullen
        char buf[9];
        snprintf(buf, sizeof(buf), "%08lu", pulse_counter);
        return String(buf);
    } else if (var == "pulse_counter_left") {
        // Linke 5 Stellen für CSS-Formatierung
        char buf[6];
        snprintf(buf, sizeof(buf), "%05lu", pulse_counter / 1000);
        return String(buf);
    } else if (var == "pulse_counter_right") {
        // Rechte 3 Stellen für CSS-Formatierung
        char buf[4];
        snprintf(buf, sizeof(buf), "%03lu", pulse_counter % 1000);
        return String(buf);
    }
    // Unbekannte Variable - leeren String zurückgeben
    return String();
}

void setupWebServer() {
    // Root-Handler mit Template-Processor
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity = millis();  // Web-Server-Aktivität aktualisieren
        
        // ADC-Werte bei jedem Seitenaufruf neu messen (für Auto-Refresh)
        battery_adc_mv = read_adc_median_mv();
        battery_voltage = (float)battery_adc_mv / 1000.0f * VOLTAGE_DIVIDER_RATIO + config_rtc.adc_voltage_offset;
        battery_percent = VOLTAGE_TO_PERCENT(battery_voltage);
        
        // Datei laden und Template-Variablen ersetzen
        File file = SPIFFS.open("/index.html", "r");
        if (!file) {
            request->send(500, "text/plain", "index.html nicht gefunden");
            return;
        }
        
        String html = file.readString();
        file.close();
        
        // Template-Variablen ersetzen
        html.replace("{{hostname}}", processor("hostname"));
        html.replace("{{wifi_ssid}}", processor("wifi_ssid"));
        html.replace("{{wakeup_minutes}}", processor("wakeup_minutes"));
        html.replace("{{transfer_minutes}}", processor("transfer_minutes"));
        html.replace("{{adc_value}}", processor("adc_value"));
        html.replace("{{battery_voltage}}", processor("battery_voltage"));
        html.replace("{{battery_percent}}", processor("battery_percent"));
        html.replace("{{ntp_server}}", processor("ntp_server"));
        html.replace("{{wakeup_count}}", processor("wakeup_count"));
        html.replace("{{wifi_status}}", processor("wifi_status"));
        html.replace("{{wifi_ip}}", processor("wifi_ip"));
        html.replace("{{wifi_rssi}}", processor("wifi_rssi"));
        html.replace("{{system_time}}", processor("system_time"));
        html.replace("{{project_name}}", processor("project_name"));
        html.replace("{{project_version}}", processor("project_version"));
        html.replace("{{build_date}}", processor("build_date"));
        html.replace("{{nv_magic_key}}", processor("nv_magic_key"));
        html.replace("{{pulse_counter}}", processor("pulse_counter"));
        html.replace("{{pulse_counter_left}}", processor("pulse_counter_left"));
        html.replace("{{pulse_counter_right}}", processor("pulse_counter_right"));
        
        // WiFi-Info anzeigen, wenn verbunden
        if (WiFi.status() == WL_CONNECTED) {
            html.replace("style=\"display:none;\"", "");
        }
        
        // HTML mit Cache-Control-Header senden (kein Caching)
        AsyncWebServerResponse *response = request->beginResponse(200, "text/html", html);
        response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        response->addHeader("Pragma", "no-cache");
        response->addHeader("Expires", "0");
        request->send(response);
    });
    
    // CSS-Datei
    server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity = millis();  // Web-Server-Aktivität aktualisieren
        
        // CSS mit Cache-Control-Header senden (kein Caching)
        AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/style.css", "text/css");
        response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        response->addHeader("Pragma", "no-cache");
        response->addHeader("Expires", "0");
        request->send(response);
    });
    
    // JavaScript-Datei
    server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity = millis();  // Web-Server-Aktivität aktualisieren
        
        // JavaScript mit Cache-Control-Header senden (kein Caching)
        AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/script.js", "application/javascript");
        response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        response->addHeader("Pragma", "no-cache");
        response->addHeader("Expires", "0");
        request->send(response);
    });
    
    // Reboot-Endpunkt
    server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *request){
        last_web_activity = millis();  // Web-Server-Aktivität aktualisieren
        request->send(200, "text/plain", "Reboot wird durchgeführt...");
        delay(500);  // Kurze Verzögerung, damit die Antwort gesendet wird
        Serial.println("Reboot durch Web-Interface ausgelöst");
        
        // WICHTIG: pulse_counter vor ESP.restart() in Ring-Speicher speichern
        // (RTC-RAM wird bei ESP.restart() zurückgesetzt)
        Serial.println("Speichere pulse_counter in Ring-Speicher vor Reboot...");
        write_pulse_counter_to_ring_buffer();
        
        Serial.flush();
        ESP.restart();
    });
    
    // 404 Handler
    server.onNotFound([](AsyncWebServerRequest *request){
        last_web_activity = millis();  // Web-Server-Aktivität aktualisieren (auch bei 404)
        request->send(404, "text/plain", "Not Found");
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
    
    Serial.println("\n=== Gas-O-Meter ===");
    Serial.printf("Wake-up Count: %d\n", wakeupCount);
    
    // Wake-up-Grund ausgeben
    switch (wakeup_reason) {
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            Serial.println("Power-On erkannt (kein Wake-up)");
            break;
        case ESP_SLEEP_WAKEUP_EXT0:
            Serial.println("Wake-up erkannt: EXT0 (GPIO)");
            break;
        case ESP_SLEEP_WAKEUP_EXT1:
            Serial.println("Wake-up erkannt: EXT1 (GPIO)");
            break;
        case ESP_SLEEP_WAKEUP_GPIO:
            Serial.println("Wake-up erkannt: GPIO (Taster A gedrückt)");
            // GPIO-Wake-up-Pin ermitteln (falls unterstützt)
            break;
        case ESP_SLEEP_WAKEUP_TIMER:
            Serial.println("Wake-up erkannt: Timer");
            break;
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
            Serial.println("Wake-up erkannt: Touchpad");
            break;
        case ESP_SLEEP_WAKEUP_ULP:
            Serial.println("Wake-up erkannt: ULP");
            break;
        default:
            Serial.printf("Wake-up erkannt: Unbekannt (0x%x)\n", wakeup_reason);
            break;
    }
    
    if (isPowerOn) {
        Serial.println("Power-On erkannt");
    } else {
        Serial.println("Wake-up erkannt");
    }
    
    // Interne LED initialisieren und sofort einschalten (HP-Core läuft)
    // WICHTIG: LED wird hier eingeschaltet, um zu zeigen, dass HP-Core aktiv ist
    pinMode(LED_BUILTIN_GPIO, OUTPUT);
    digitalWrite(LED_BUILTIN_GPIO, LED_ON);  // LED EIN (HP-Core aktiv)
    Serial.printf("Interne LED initialisiert (GPIO%d) und eingeschaltet (ON=%s)\n", 
                 LED_BUILTIN_GPIO, LED_ON == HIGH ? "HIGH" : "LOW");
    
    // ADC initialisieren (analogReadMilliVolts - automatisch kalibriert)
    // WICHTIG: Spannungsprüfung VOR NVS-Initialisierung, um Akku zu schützen!
    esp_err_t adc_ret = init_adc();
    if (adc_ret != ESP_OK) {
        Serial.printf("ADC Initialisierung fehlgeschlagen: %s\n", esp_err_to_name(adc_ret));
        // Auch ohne ADC: LED einschalten (HP-Core läuft)
        pinMode(LED_BUILTIN_GPIO, OUTPUT);
        digitalWrite(LED_BUILTIN_GPIO, LED_ON);
        Serial.println("Interne LED eingeschaltet (HP-Core aktiv, ADC-Fehler ignoriert)");
    } else {
        // Akku-Messung (analogReadMilliVolts gibt direkt Millivolt zurück)
        battery_adc_mv = read_adc_median_mv();
        battery_voltage = (float)battery_adc_mv / 1000.0f * VOLTAGE_DIVIDER_RATIO + config_rtc.adc_voltage_offset;
        battery_percent = VOLTAGE_TO_PERCENT(battery_voltage);
        
        Serial.printf("ADC: %d mV, Spannung: %.2f V, Prozent: %d%%\n", 
                     battery_adc_mv, battery_voltage, battery_percent);
        
        // Stromversorgungs-Prüfung und Akku-Schutz (SOFORT, vor allen anderen Operationen)
        if (battery_voltage < USB_DETECTION_THRESHOLD) {
            // Spannung < 2V: USB-Stromversorgung angeschlossen (ESP32C6 würde sonst nicht starten)
            Serial.println("USB-Stromversorgung erkannt - Betrieb fortgesetzt");
            Serial.printf("Spannung: %.2f V (USB-Schwelle: %.2f V)\n", 
                         battery_voltage, USB_DETECTION_THRESHOLD);
            // WICHTIG: pulse_counter wird vor Deep-Sleep gespeichert (wie bei Akku-Low-Power)
            // (USB-Stecker kann jederzeit gezogen werden → RTC-RAM geht verloren)
        } else if (battery_voltage < BATTERY_VOLTAGE_20) {
            // Spannung >= 2V aber < BATTERY_VOLTAGE_20: Akku zu niedrig → Deep-Sleep zum Schutz
            Serial.println("Akku zu niedrig - Deep-Sleep zum Akku-Schutz");
            Serial.printf("Spannung: %.2f V (Minimum: %.2f V)\n", 
                         battery_voltage, BATTERY_VOLTAGE_20);
            
            // WICHTIG: pulse_counter vor Deep-Sleep in Ring-Speicher speichern
            // (bei Akku-Low kann RTC-RAM verloren gehen)
            Serial.println("Speichere pulse_counter in Ring-Speicher vor Deep-Sleep (Akku-Low)...");
            write_pulse_counter_to_ring_buffer();
            
            Serial.println("Gehe in Deep-Sleep mit Taster A Wake-up...");
            Serial.flush();
            
            // Deep-Sleep mit GPIO-Wake-up (Taster A) - spart Energie und schützt Akku
            enter_deep_sleep_with_gpio_wakeup();
            // Ab hier wird Code nicht mehr ausgeführt (Deep-Sleep)
        }
        
        // Ab hier: Spannung ist OK, HP-Core läuft → LED sicherstellen, dass sie eingeschaltet ist
        // (LED sollte bereits eingeschaltet sein, aber zur Sicherheit nochmal setzen)
        digitalWrite(LED_BUILTIN_GPIO, LED_ON);  // LED EIN (HP-Core aktiv)
        Serial.println("Interne LED Status bestätigt (HP-Core aktiv, Spannung OK)");
        
        // Nur wenn Spannung OK: NVS initialisieren (bei Power-On)
        if (isPowerOn) {
            // NVS initialisieren (beim ersten Boot)
            // Standard-NVS-Partition initialisieren
            esp_err_t nvs_err = nvs_flash_init();
            if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
                // NVS-Partition wurde gelöscht oder hat neue Version - neu initialisieren
                Serial.println("Standard-NVS-Partition muss neu initialisiert werden...");
                esp_err_t erase_err = nvs_flash_erase();
                if (erase_err != ESP_OK) {
                    Serial.printf("NVS-Löschung fehlgeschlagen: %s\n", esp_err_to_name(erase_err));
                } else {
                    nvs_err = nvs_flash_init();
                }
            }
            if (nvs_err != ESP_OK) {
                Serial.printf("Standard-NVS-Initialisierung fehlgeschlagen: %s\n", esp_err_to_name(nvs_err));
            } else {
                Serial.println("Standard-NVS erfolgreich initialisiert");
            }
            
            // Pulse-NVS-Partition initialisieren
            esp_err_t pulse_nvs_err = nvs_flash_init_partition(NVS_PARTITION_PULSE);
            if (pulse_nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || pulse_nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
                // Pulse-NVS-Partition wurde gelöscht oder hat neue Version - neu initialisieren
                Serial.printf("Pulse-NVS-Partition '%s' muss neu initialisiert werden...\n", NVS_PARTITION_PULSE);
                esp_err_t erase_err = nvs_flash_erase_partition(NVS_PARTITION_PULSE);
                if (erase_err != ESP_OK) {
                    Serial.printf("Pulse-NVS-Löschung fehlgeschlagen: %s\n", esp_err_to_name(erase_err));
                } else {
                    pulse_nvs_err = nvs_flash_init_partition(NVS_PARTITION_PULSE);
                }
            }
            if (pulse_nvs_err != ESP_OK) {
                Serial.printf("Pulse-NVS-Initialisierung fehlgeschlagen: %s\n", esp_err_to_name(pulse_nvs_err));
            } else {
                Serial.printf("Pulse-NVS-Partition '%s' erfolgreich initialisiert\n", NVS_PARTITION_PULSE);
            }
            
            // NVS-Ring-Speicher: Versionsnummer prüfen und ggf. initialisieren
            // Dies geschieht NUR beim ersten Boot nach Code-Upload oder partition.csv-Änderung
            Serial.println("Prüfe NVS-Ring-Speicher-Version...");
            Serial.printf("Erwartete Version (Build-Timestamp): %lu\n", RING_BUFFER_VERSION);
            check_and_init_pulse_ring_nvs();
            
            // LP-Core Watchdog Task starten (asynchron)
            // Der Task prüft automatisch lp_core_running und startet LP-Core bei Bedarf
            xTaskCreate(
                lp_core_watchdog_task,      // Task-Funktion
                "LP_Core_Watchdog",          // Task-Name
                4096,                        // Stack-Größe (Bytes)
                NULL,                        // Parameter
                1,                           // Priorität (niedrig, da nicht kritisch)
                NULL                         // Task-Handle (nicht benötigt)
            );
            Serial.println("LP-Core Watchdog Task gestartet");
            
            // ring_idx aus Ring-Speicher ermitteln (RTC-RAM wurde bei Power-On/ESP.restart() zurückgesetzt)
            uint32_t max_index = 0;
            uint32_t max_pulse = find_max_pulse_and_index_from_nvs(&max_index);
            if (max_pulse > 0) {
                // Nächster freier Slot: (max_index + 1) % RING_BUFFER_SIZE
                ring_idx = (max_index + 1) % RING_BUFFER_SIZE;
                Serial.printf("ring_idx aus Ring-Speicher ermittelt: %lu (höchster Index: %lu)\n", ring_idx, max_index);
            } else {
                ring_idx = 0;  // Erster Slot (keine Daten vorhanden)
                Serial.println("ring_idx auf 0 gesetzt (keine Daten im Ring-Speicher)");
            }
            
            // Beim ersten Boot (Power-On) oder nach ESP.restart(): pulse_counter aus Ring-Speicher laden
            // WICHTIG: RTC-RAM ist bei Power-On/ESP.restart() leer (pulse_counter == 0)
            // Bei Deep-Sleep-Wake-up ist RTC-RAM noch vorhanden und muss NICHT geladen werden
            if (pulse_counter == 0) {
                Serial.println("RTC-RAM leer → Lade pulse_counter aus Ring-Speicher...");
                if (max_pulse > 0) {
                    pulse_counter = max_pulse;
                    Serial.printf("pulse_counter aus Ring-Speicher übernommen: %lu\n", pulse_counter);
                } else {
                    pulse_counter = 0;
                    Serial.println("Keine Ring-Speicher-Daten gefunden, pulse_counter auf 0 initialisiert");
                }
            } else {
                Serial.printf("RTC-RAM noch vorhanden (pulse_counter = %lu) → Keine NVS-Übertragung nötig\n", pulse_counter);
                Serial.println("RTC-RAM behält Daten bei Deep-Sleep-Wake-up");
            }
        } else {
            // Bei Deep-Sleep-Wake-up: RTC-RAM sollte noch vorhanden sein
            Serial.printf("Deep-Sleep-Wake-up erkannt → RTC-RAM sollte noch vorhanden sein (pulse_counter = %lu, ring_idx = %lu)\n", 
                         pulse_counter, ring_idx);
            if (pulse_counter == 0) {
                Serial.println("WARNUNG: RTC-RAM ist leer trotz Deep-Sleep-Wake-up!");
                Serial.println("Mögliche Ursachen: Spannung war unterbrochen oder RTC-RAM wurde zurückgesetzt");
                Serial.println("Versuche pulse_counter und ring_idx aus Ring-Speicher zu laden...");
                
                // Versuche aus Ring-Speicher zu laden
                uint32_t max_index = 0;
                uint32_t max_pulse = find_max_pulse_and_index_from_nvs(&max_index);
                if (max_pulse > 0) {
                    pulse_counter = max_pulse;
                    ring_idx = (max_index + 1) % RING_BUFFER_SIZE;
                    Serial.printf("pulse_counter und ring_idx aus Ring-Speicher wiederhergestellt: pulse=%lu, ring_idx=%lu\n", 
                                 pulse_counter, ring_idx);
                }
            } else if (ring_idx >= RING_BUFFER_SIZE) {
                // ring_idx ist ungültig, aber pulse_counter ist vorhanden
                Serial.println("WARNUNG: ring_idx ist ungültig trotz Deep-Sleep-Wake-up!");
                Serial.println("Ermittle ring_idx aus Ring-Speicher...");
                uint32_t max_index = 0;
                find_max_pulse_and_index_from_nvs(&max_index);
                ring_idx = (max_index + 1) % RING_BUFFER_SIZE;
                Serial.printf("ring_idx wiederhergestellt: %lu\n", ring_idx);
            }
        }
        
        // Config laden, WiFi, NTP und Web-Server (nur wenn Spannung OK)
        if (load_config()) {
            Serial.println("Config erfolgreich geladen");
            
            // WiFi verbinden
            if (connect_wifi()) {
                // NTP-Zeitsynchronisation
                sync_ntp_time();
                
                // Web-Server starten
                setupWebServer();
                
                Serial.println("Web-Server gestartet");
                Serial.printf("Öffne: http://%s\n", WiFi.localIP().toString().c_str());
            } else {
                Serial.println("WiFi-Verbindung fehlgeschlagen");
            }
        } else {
            Serial.println("Config-Laden fehlgeschlagen");
        }
    }
}

void loop() {
    // AsyncWebServer läuft im Hintergrund, kein handleClient() nötig
    
    // Prüfe Web-Server-Inaktivität: Wenn WIFI_WAIT_FOR_SLEEP Minuten keine Aktivität → Deep-Sleep
    if (last_web_activity > 0) {
        unsigned long inactivity_ms = millis() - last_web_activity;
        unsigned long sleep_threshold_ms = WIFI_WAIT_FOR_SLEEP * 60 * 1000UL;  // Minuten in Millisekunden
        
        if (inactivity_ms >= sleep_threshold_ms) {
            Serial.printf("Keine Web-Server-Aktivität seit %lu Minuten → Deep-Sleep\n", WIFI_WAIT_FOR_SLEEP);
            
            // WICHTIG: pulse_counter vor Deep-Sleep in Ring-Speicher speichern
            // (gilt für alle Deep-Sleep-Szenarien: Inaktivität, Akku-Low, USB-Betrieb)
            Serial.println("Speichere pulse_counter in Ring-Speicher vor Deep-Sleep...");
            write_pulse_counter_to_ring_buffer();
            
            Serial.flush();
            
            // Deep-Sleep mit GPIO-Wake-up (Taster A)
            enter_deep_sleep_with_gpio_wakeup();
        }
    }
    
    delay(100);
}

