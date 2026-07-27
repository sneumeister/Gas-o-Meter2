// ESP-IDF Headers
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "esp_littlefs.h"
#include "lwip/apps/sntp.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "captive_portal.h"
#include <string.h>
#include "esp_vfs.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "ulp_lp_core.h"  // LP-Core Management APIs
#include <time.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "hardware.h"
#include "version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "transfer.h"
#include "transfer_zigbee.h"
#include "zigbee_config.h"
#include "transfer_ble.h"
#include "ble_config.h"
#include "mqtt_config.h"
#include "transfer_mqtt.h"
#include "wifi_scan_config.h"
#include "wifi_manager.h"
#include "time_sync.h"
#include <stdlib.h>

#include <ArduinoJson.h>

// Template-Placeholder (aus CMakeLists.txt)
#ifndef TEMPLATE_PLACEHOLDER
#define TEMPLATE_PLACEHOLDER '`'   // Backtick (`) für Template-Platzhalter
#endif

// Logging-Tag
static const char *TAG = "gas-o-meter";

// LP-Core Binary Header (generiert durch ulp_embed_binary in src/CMakeLists.txt)
// ulp_embed_binary() erstellt automatisch ulp_main.h mit:
//   - Binary-Symbolen: ulp_main_bin_start, ulp_main_bin_end (für ulp_lp_core_load_binary())
//   - Exportierten globalen Variablen aus LP-Core Code mit "ulp_" Präfix:
//       extern uint32_t ulp_pulse_counter;      // Siehe Kommentar bei Zeile 135
//       extern uint32_t ulp_lp_core_running;     // Siehe Kommentar bei Zeile 135
// ulp_app_name="ulp_main" → Header-File: ulp_main.h
// WICHTIG: Keine expliziten extern-Deklarationen nötig - alles erfolgt implizit über dieses Include!
// WICHTIG: Alle Zugriffe auf ULP-Variablen müssen mit volatile-Casts erfolgen (siehe Kommentar bei Zeile 135)
#include "ulp_main.h"
// Manuelle Deklaration für das LP-Core Programm-Image
extern "C" {
    extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_main_bin_start");
    extern const uint8_t ulp_main_bin_end[]   asm("_binary_ulp_main_bin_end");
}

// LP-Core Initialisierung und Start
// Läuft auf dem HP-Core und startet den LP-Core-Prozessor
bool start_lp_core(void) {
    ESP_LOGI(TAG, "Starte LP-Core...");

    // Binary-Load initialisiert LP-BSS neu (pulse_counter := 0). Stand vorher sichern.
    const uint32_t saved_pulse = *(volatile uint32_t *)&ulp_pulse_counter;

    // Watchdog-Zähler zurücksetzen — sonst interpretiert der HP-Core RTC-Müll als „LP läuft“
    *(volatile uint32_t *)&ulp_lp_core_running = 0;
    
    // REED-Pin als RTC-GPIO initialisieren (erforderlich für LP-Core-Zugriff)
    // GPIO2 auf ESP32C6
    rtc_gpio_init((gpio_num_t)REED_GPIO);
    rtc_gpio_set_direction((gpio_num_t)REED_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    // Kein Pull-Up/Pull-Down (externer Pull-Up) - beide Pulls deaktivieren = Float
    rtc_gpio_pullup_dis((gpio_num_t)REED_GPIO);
    rtc_gpio_pulldown_dis((gpio_num_t)REED_GPIO);
    
    ESP_LOGI(TAG, "REED-Pin (GPIO%d) als RTC-GPIO initialisiert", REED_GPIO);
    
    // LP-Core Binary laden
    // Binary-Symbole werden durch ulp_main.h (generiert von ulp_embed_binary) deklariert
    // Format: _binary_ulp_<ulp_app_name>_bin_start/end mit ulp_app_name="ulp_main"
    size_t binary_size = ulp_main_bin_end - ulp_main_bin_start;
    esp_err_t ret = ulp_lp_core_load_binary(ulp_main_bin_start, binary_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FEHLER: LP-Core Binary konnte nicht geladen werden: %s", esp_err_to_name(ret));
        return false;
    }
    
    ESP_LOGI(TAG, "LP-Core Binary geladen (%zu Bytes)", binary_size);

    // Zählerstand wiederherstellen (NVS/RTC-Wert darf durch Load nicht verloren gehen)
    *(volatile uint32_t *)&ulp_pulse_counter = saved_pulse;
    ESP_LOGI(TAG, "LP-Core: ulp_pulse_counter nach Load wiederhergestellt: %lu",
             (unsigned long)saved_pulse);
    
    // LP-Core konfigurieren und starten
    ulp_lp_core_cfg_t cfg = {
        .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU,  // Wird vom HP-Core geweckt
    };
    
    ret = ulp_lp_core_run(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FEHLER: LP-Core konnte nicht gestartet werden: %s", esp_err_to_name(ret));
        return false;
    }
    
    // LP-Core starten (Software-Interrupt)
    ulp_lp_core_sw_intr_trigger();
    
    ESP_LOGI(TAG, "LP-Core gestartet");
    return true;
}


// ============================================
// RTC Memory Struktur für Config-Werte
// ============================================
typedef struct {
    char hostname[HOSTNAME_MAX_LEN + 1];
    char adminpass[32];
    struct {
        char ssid[32];
        char password[64];
    } wifi_credentials[2];  // Max 2 SSID/Passwort-Paare
    uint8_t wifi_count;  // Anzahl der gespeicherten Credentials (0, 1 oder 2)
    uint8_t wakeup_minutes;
    uint8_t transfer_minutes;
    char transfer_mode[16];  // "zigbee", "ble", "mqtt", "none" (Default: "none")
    int8_t wifi_tx_power_dbm;     // WiFi TX Power in dBm (2..20 in UI-Stufen)
    int8_t ble_tx_power_dbm;      // BLE TX Power in dBm (3/6/9/12/15/18/20)
    int8_t zigbee_tx_power_dbm;   // ZigBee TX Power in dBm (-9/-6/-3/0/+3/+6/+10)
    float adc_voltage_multiplier;  // ADC-Skalierung (aus config.json oder hardware.h)
    char ntp_server[64];       // NTP-Server (aus config.json oder hardware.h)
    char mqtt_host[MQTT_HOST_MAX_LEN + 1];
    uint16_t mqtt_port;
    char mqtt_username[MQTT_USERNAME_MAX_LEN + 1];
    char mqtt_password[MQTT_PASSWORD_MAX_LEN + 1];
    char mqtt_main_topic[MQTT_MAIN_TOPIC_MAX_LEN + 1];
    bool mqtt_ha_autodiscovery;
    bool config_loaded;
} config_rtc_t;

RTC_DATA_ATTR config_rtc_t config_rtc = {
    .hostname = "gasometer2",
    .adminpass = "",
    .wifi_credentials = {{"", ""}, {"", ""}},
    .wifi_count = 0,
    .wakeup_minutes = DEFAULT_WAKEUP_INTERVAL_MIN,
    .transfer_minutes = DEFAULT_TRANSFER_INTERVAL_X * DEFAULT_WAKEUP_INTERVAL_MIN,
    .transfer_mode = "none",  // Default: keine Übertragung
    .wifi_tx_power_dbm = (int8_t)(WIFI_TX_POWER_DEFAULT / 4), // Default: 20 dBm
    .ble_tx_power_dbm = (int8_t)BLE_TX_POWER_DBM,               // Default: 9 dBm
    .zigbee_tx_power_dbm = (int8_t)ZIGBEE_TX_POWER_DEFAULT,     // Default: 0 dBm
    .adc_voltage_multiplier = ADC_VOLTAGE_MULTIPLIER,  // Default aus hardware.h
    .ntp_server = DEFAULT_NTP_SERVER,          // Default aus hardware.h
    .mqtt_host = MQTT_DUMMY_HOST,
    .mqtt_port = MQTT_DEFAULT_PORT,
    .mqtt_username = "",
    .mqtt_password = "",
    .mqtt_main_topic = MQTT_DEFAULT_MAIN_TOPIC,
    .mqtt_ha_autodiscovery = false,
    .config_loaded = false
};
RTC_DATA_ATTR int wakeupCount = 0;  // Zählt nur Deep-Sleep-Wake-ups (nicht ESP.restart())
RTC_DATA_ATTR uint32_t timer_wake_count = 0;  // Nur Timer-Wake-ups (für Übertragungs-Intervall)
RTC_DATA_ATTR bool isPowerOn = false;

// Hostname-Zugriff für transfer_ble.cpp (BLE Device Name = Hostname)
const char* transfer_ble_get_hostname(void) {
    return config_rtc.hostname;
}

// TX Power-Zugriff für transfer_ble.cpp
int8_t transfer_ble_get_tx_power_dbm(void) {
    return config_rtc.ble_tx_power_dbm;
}

// TX Power-Zugriff für transfer_zigbee.cpp
int8_t transfer_zigbee_get_tx_power_dbm(void) {
    return config_rtc.zigbee_tx_power_dbm;
}

const char* transfer_mqtt_get_host(void) {
    return config_rtc.mqtt_host;
}

uint16_t transfer_mqtt_get_port(void) {
    return config_rtc.mqtt_port;
}

const char* transfer_mqtt_get_username(void) {
    return config_rtc.mqtt_username;
}

const char* transfer_mqtt_get_password(void) {
    return config_rtc.mqtt_password;
}

const char* transfer_mqtt_get_main_topic(void) {
    return config_rtc.mqtt_main_topic;
}

bool transfer_mqtt_get_ha_autodiscovery(void) {
    return config_rtc.mqtt_ha_autodiscovery;
}

const char* transfer_mqtt_get_hostname(void) {
    return config_rtc.hostname;
}

extern "C" bool wifi_manager_load_sta_config(wifi_manager_sta_config_t* out) {
    if (out == nullptr) {
        return false;
    }
    out->wifi_count = config_rtc.wifi_count;
    out->wifi_tx_power_dbm = config_rtc.wifi_tx_power_dbm;
    strncpy(out->hostname, config_rtc.hostname, sizeof(out->hostname) - 1);
    out->hostname[sizeof(out->hostname) - 1] = '\0';
    for (uint8_t i = 0; i < 2; i++) {
        strncpy(out->ssid[i], config_rtc.wifi_credentials[i].ssid, sizeof(out->ssid[i]) - 1);
        out->ssid[i][sizeof(out->ssid[i]) - 1] = '\0';
        strncpy(out->password[i], config_rtc.wifi_credentials[i].password, sizeof(out->password[i]) - 1);
        out->password[i][sizeof(out->password[i]) - 1] = '\0';
    }
    return true;
}
RTC_DATA_ATTR uint32_t ring_idx = RING_BUFFER_SIZE;  // Ring-Buffer-Index (im RTC-RAM, wird bei Power-On/ESP.restart() neu ermittelt)

// ============================================
// LP-Core Variablen (pulse_counter, lp_core_running)
// ============================================
// EHEMALIGE DEFINITIONEN (entfernt, jetzt im LP-Core Code):
//   RTC_DATA_ATTR uint32_t pulse_counter = 0;      // Puls-Zähler für LP-Core
//   RTC_DATA_ATTR uint32_t lp_core_running = 0;     // LP-Core Watchdog-Zähler
//
// AKTUELLE IMPLEMENTIERUNG:
//   - Variablen sind jetzt in ulp/ulp_main.c definiert:
//       volatile uint32_t pulse_counter = 0;      // volatile wegen gleichzeitigem Zugriff
//       volatile uint32_t lp_core_running = 0;     // volatile wegen gleichzeitigem Zugriff
//   - ulp_embed_binary() exportiert sie automatisch mit "ulp_" Präfix über ulp_main.h:
//       extern uint32_t ulp_pulse_counter;      // WICHTIG: volatile fehlt hier!
//       extern uint32_t ulp_lp_core_running;    // WICHTIG: volatile fehlt hier!
//   - Die Deklarationen erfolgen implizit durch #include "ulp_main.h" (siehe Zeile 47)
//   - WICHTIG: Alle Zugriffe auf diese Variablen müssen mit volatile-Casts erfolgen:
//       uint32_t val = *(volatile uint32_t *)&ulp_pulse_counter;  // Lesen
//       *(volatile uint32_t *)&ulp_pulse_counter = new_val;        // Schreiben
//     Dies zwingt den Compiler, immer vom RAM zu lesen/schreiben und verhindert Race Conditions
//   - Im Code werden die Variablen mit "ulp_" Präfix verwendet (z.B. ulp_pulse_counter)
//   - Die Variablen liegen im RTC-RAM und werden zwischen HP-Core und LP-Core geteilt

// ============================================
// Globale Variablen
// ============================================
httpd_handle_t server = NULL;  // ESP-IDF HTTP Server Handle
bool littlefs_mounted = false;
bool server_started = false;  // Flag: Web-Server gestartet?
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

// Reboot-Steuerung: Flag setzen, Ausführung in web_timeout_task
bool reboot_requested = false;
const char* reboot_reason = NULL;

// Funktionsdeklarationen
void web_timeout_task(void *parameter);
void enter_deep_sleep_with_gpio_and_timer_wakeup(bool enable_timer);
bool write_ulp_pulse_counter_to_ring_buffer();

static bool wake_allows_web_ui(esp_sleep_wakeup_cause_t reason) {
    return reason == ESP_SLEEP_WAKEUP_GPIO || reason == ESP_SLEEP_WAKEUP_EXT0 ||
           reason == ESP_SLEEP_WAKEUP_EXT1 || reason == ESP_SLEEP_WAKEUP_UNDEFINED;
}

/** true = jetzt transfer_data() ausführen (Timer-Wake-up, Intervall aus Config). */
static bool timer_wake_should_transfer(bool config_available) {
    if (!config_available || config_rtc.transfer_minutes == 255) {
        return false;
    }
    if (config_rtc.wakeup_minutes == 0) {
        return false;
    }
    const uint8_t every_n = (uint8_t)(config_rtc.transfer_minutes / config_rtc.wakeup_minutes);
    const uint8_t interval = (every_n == 0) ? 1 : every_n;
    return (timer_wake_count % interval) == 0;
}

static void persist_pulse_counter_before_deep_sleep(void) {
    if (battery_voltage < BATTERY_VOLTAGE_30 || IS_USB_POWER(battery_voltage)) {
        ESP_LOGI(TAG, "Speichere ulp_pulse_counter in Ring-Speicher vor Deep-Sleep (< 30%% oder USB)...");
        write_ulp_pulse_counter_to_ring_buffer();
    } else {
        ESP_LOGI(TAG, "Akku-Spannung OK (>= 30%%) → ulp_pulse_counter bleibt im RTC-RAM (kein Schreiben nötig)");
    }
}

static void enter_deep_sleep_after_wakeup(bool enable_timer_wakeup) {
    persist_pulse_counter_before_deep_sleep();
    // ZigBee: CAN_SLEEP -> deinit -> WiFi in shutdown_resources(true)
    enter_deep_sleep_with_gpio_and_timer_wakeup(enable_timer_wakeup);
}

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
        .base_path = "/littlefs",
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
    
    // Partitionsgröße ausgeben
    size_t total_bytes = 0;
    size_t used_bytes = 0;
    ret = esp_littlefs_info("storage", &total_bytes, &used_bytes);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS Partition: %zu KB gesamt, %zu KB verwendet (%.1f%%)", 
                 total_bytes / 1024, used_bytes / 1024, 
                 (float)used_bytes * 100.0f / (float)total_bytes);
    } else {
        ESP_LOGW(TAG, "LittleFS Info konnte nicht abgerufen werden: %s", esp_err_to_name(ret));
    }
    
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
// RTC ulp_pulse_counter: In Ring-Speicher schreiben (bei ESP.restart(), Akku-Low, USB)
// ============================================
bool write_ulp_pulse_counter_to_ring_buffer() {
    // WICHTIG: Stelle sicher, dass Pulse-NVS initialisiert ist
    if (!init_pulse_nvs_minimal()) {
        ESP_LOGE(TAG, "FEHLER: Pulse-NVS konnte nicht initialisiert werden → kein Schreiben möglich");
        return false;
    }
    
    // Prüfung: Nur schreiben, wenn ulp_pulse_counter > 0
    if (*(volatile uint32_t *)&ulp_pulse_counter == 0) {
        ESP_LOGI(TAG, "ulp_pulse_counter ist 0 → Keine Ring-Speicher-Schreibung nötig");
        return true;  // Kein Fehler, einfach nichts zu speichern
    }
    
    // Prüfung: Nur schreiben, wenn ulp_pulse_counter > max_pulse aus Ring-Speicher
    uint32_t max_pulse = find_max_pulse_from_nvs();
    uint32_t current_pulse = *(volatile uint32_t *)&ulp_pulse_counter;
    if (current_pulse <= max_pulse) {
        ESP_LOGI(TAG, "ulp_pulse_counter nicht gespeichert: RTC=%lu <= Ring-Speicher-Max=%lu", 
                 current_pulse, max_pulse);
        return true;  // Kein Fehler, einfach nichts zu speichern
    }
    
    // Ring-Speicher öffnen
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open_from_partition(NVS_PARTITION_PULSE, NVS_NAMESPACE_PULSE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fehler beim Öffnen von Ring-Speicher für ulp_pulse_counter: %s", esp_err_to_name(err));
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
    
    // ulp_pulse_counter an Position ring_idx schreiben
    char key[MAX_KEY_LENGTH];
    snprintf(key, sizeof(key), "%s%lu", NVS_KEY_PREFIX, ring_idx);
    err = nvs_set_u32(nvs_handle, key, *(volatile uint32_t *)&ulp_pulse_counter);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fehler beim Schreiben von ulp_pulse_counter in Ring-Speicher: %s", esp_err_to_name(err));
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
    
    ESP_LOGI(TAG, "ulp_pulse_counter in Ring-Speicher geschrieben: %lu (Position: %lu, nächster Index: %lu)", 
             *(volatile uint32_t *)&ulp_pulse_counter, (ring_idx == 0 ? RING_BUFFER_SIZE - 1 : ring_idx - 1), ring_idx);
    return true;
}


// FreeRTOS Task für LP-Core Watchdog
static bool lp_core_running_sane(uint32_t value)
{
    return value <= LP_CORE_RUNNING_SANITY_MAX;
}

void lp_core_watchdog_task(void *parameter) {
    uint32_t last_lp_core_value = 0;
    uint8_t retry_count = 0;
    const uint8_t MAX_RETRIES = 3;
    
    ESP_LOGI(TAG, "LP-Core Watchdog Task gestartet");

    uint32_t running = *(volatile uint32_t *)&ulp_lp_core_running;
    if (!lp_core_running_sane(running)) {
        ESP_LOGW(TAG, "ulp_lp_core_running=%lu (ungültig, kein LP-Start) → zurücksetzen",
                 (unsigned long)running);
        *(volatile uint32_t *)&ulp_lp_core_running = 0;
    } else if (running > 0) {
        ESP_LOGI(TAG, "LP-Core Watchdog: vorhandener Zähler %lu (Prüfung folgt)",
                 (unsigned long)running);
    }
    
    // Initialisiere last_lp_core_value mit aktuellem Wert
    last_lp_core_value = *(volatile uint32_t *)&ulp_lp_core_running;
    
    // Kombinierte Start- und Watchdog-Schleife
    // Wenn ulp_lp_core_running == 0 ODER Counter erhöht sich nicht → versuche LP-Core zu starten
    // Wenn nach MAX_RETRIES immer noch nicht erfolgreich → Task beenden
    while (1) {
        // Prüfe ob LP-Core läuft (ulp_lp_core_running == 0 bedeutet: nicht gestartet oder gestoppt)
    if (*(volatile uint32_t *)&ulp_lp_core_running == 0) {
            // LP-Core läuft nicht → versuche zu starten
            retry_count++;
            ESP_LOGW(TAG, "LP-Core läuft nicht (ulp_lp_core_running == 0) → Starte LP-Core... (Versuch %d/%d)", 
                     retry_count, MAX_RETRIES);
            
            if (retry_count >= MAX_RETRIES) {
                ESP_LOGE(TAG, "FEHLER: LP-Core konnte nach %d Versuchen nicht gestartet werden. Watch-Dog-Task beendet!", MAX_RETRIES);
            vTaskDelete(NULL);
            return;
        }
            
            // Versuche LP-Core zu starten
            if (start_lp_core()) {
                // start_lp_core() gab true zurück - warte auf Watchdog-Timeout und prüfe dann
        last_lp_core_value = *(volatile uint32_t *)&ulp_lp_core_running;
                ESP_LOGI(TAG, "LP-Core Start aufgerufen (Zähler: %lu) - warte auf Watchdog-Timeout für Prüfung...", last_lp_core_value);
            } else {
                // start_lp_core() gab false zurück
                ESP_LOGE(TAG, "FEHLER: LP-Core Start fehlgeschlagen (start_lp_core() gab false zurück)!");
                // Kurz warten und erneut versuchen
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        } else {
            // LP-Core sollte laufen (ulp_lp_core_running > 0) → Watchdog-Prüfung
            // Warte LP_CORE_WATCHDOG_MS bevor Prüfung (gibt LP-Core Zeit, Counter zu erhöhen)
        vTaskDelay(pdMS_TO_TICKS(LP_CORE_WATCHDOG_MS));
        
        uint32_t current_lp_core_value = *(volatile uint32_t *)&ulp_lp_core_running;
        
        // Prüfe ob Zähler sich erhöht hat
        if (current_lp_core_value == last_lp_core_value) {
            // Zähler hat sich nicht erhöht → LP-Core läuft nicht mehr!
            ESP_LOGW(TAG, "WARNUNG: LP-Core Watchdog-Timeout! (Zähler: %lu, erwartet: > %lu)", 
                     current_lp_core_value, last_lp_core_value);
                ESP_LOGI(TAG, "Setze ulp_lp_core_running auf 0 und versuche LP-Core neu zu starten...");
            
                // Setze ulp_lp_core_running auf 0, damit wir in die Start-Schleife kommen
                *(volatile uint32_t *)&ulp_lp_core_running = 0;
                retry_count = 0;  // Reset Retry-Counter für Neustart-Versuche
                continue;  // Gehe zurück in Start-Schleife
            } else {
                // Zähler hat sich erhöht → LP-Core läuft korrekt
            // ESP_LOGI(TAG, "LP-Core Watchdog OK (Zähler: %lu → %lu)", 
            //         last_lp_core_value, current_lp_core_value);
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
        // Versionsnummer stimmt nicht überein → Code wurde neu hochgeladen
        // Ring-Speicher löschen und auf 0 setzen (neuer Code = neuer Start)
        ESP_LOGI(TAG, "Versionsnummer stimmt nicht überein (gespeichert: %lu, erwartet: %lu) → Code-Upload erkannt, Ring-Speicher wird gelöscht",
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
    ESP_LOGI(TAG, "init_nvs_partition: Initialisiere %s (is_power_on=%s, is_standard_nvs=%s)...", 
             partition_name, is_power_on ? "true" : "false", is_standard_nvs ? "true" : "false");
    if (is_standard_nvs) {
        err = nvs_flash_init();
    } else {
        err = nvs_flash_init_partition(partition_name);
    }
    ESP_LOGI(TAG, "init_nvs_partition: %s Initialisierung: %s", partition_name, esp_err_to_name(err));
    
    // Prüfen, ob Neuinitialisierung erforderlich ist
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "init_nvs_partition: %s benötigt Neuinitialisierung (err=%s)", partition_name, esp_err_to_name(err));
        if (is_power_on) {
            // Bei Power-On: Partition löschen und neu initialisieren
            ESP_LOGI(TAG, "init_nvs_partition: %s muss neu initialisiert werden (Power-On → Partition wird gelöscht)...", partition_name);
            esp_err_t erase_err = is_standard_nvs ? 
                nvs_flash_erase() : 
                nvs_flash_erase_partition(partition_name);
            if (erase_err == ESP_OK) {
                ESP_LOGI(TAG, "init_nvs_partition: %s erfolgreich gelöscht", partition_name);
                err = is_standard_nvs ? 
                    nvs_flash_init() : 
                    nvs_flash_init_partition(partition_name);
                ESP_LOGI(TAG, "init_nvs_partition: %s nach Löschung neu initialisiert: %s", partition_name, esp_err_to_name(err));
            } else {
                ESP_LOGE(TAG, "init_nvs_partition: %s-Löschung fehlgeschlagen: %s", partition_name, esp_err_to_name(erase_err));
            }
        } else {
            // Bei Deep-Sleep-Wake-up: Versuche erneut ohne Löschung (NVS sollte noch vorhanden sein)
            ESP_LOGW(TAG, "init_nvs_partition: WARNUNG: %s benötigt Neuinitialisierung bei Deep-Sleep-Wake-up!", partition_name);
            ESP_LOGI(TAG, "init_nvs_partition: Versuche erneut zu initialisieren (ohne Löschung - Daten sollten erhalten bleiben)...");
            err = is_standard_nvs ? 
                nvs_flash_init() : 
                nvs_flash_init_partition(partition_name);
            ESP_LOGI(TAG, "init_nvs_partition: %s erneute Initialisierung (ohne Löschung): %s", partition_name, esp_err_to_name(err));
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
// ring_idx und ulp_pulse_counter initialisieren (kombiniert)
// ============================================
// Initialisiert ring_idx und ulp_pulse_counter aus RTC-RAM oder NVS-Ring-Speicher
// ring_idx: Ring-Buffer-Index (im RTC-RAM, wird bei Power-On/ESP.restart() neu ermittelt)
//           Bei Deep-Sleep-Wake-up sollte ring_idx noch im RTC-RAM vorhanden sein
// ulp_pulse_counter: Puls-Zähler (im RTC-RAM, wird bei Power-On/ESP.restart() aus Ring-Speicher geladen)
//                Bei Deep-Sleep-Wake-up ist RTC-RAM noch vorhanden und muss NICHT geladen werden
void init_ring_buffer_and_ulp_pulse_counter(bool is_power_on) {
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
    
    // ulp_pulse_counter initialisieren
    // WICHTIG: Bei Power-On ist RTC-RAM IMMER leer/uninitialisiert (auch wenn zufällige Werte drin stehen)
    //          → IMMER aus NVS laden
    //          Bei Deep-Sleep-Wake-up ist RTC-RAM noch vorhanden → aus RTC-RAM verwenden
    uint32_t current_pulse = *(volatile uint32_t *)&ulp_pulse_counter;
    
    if (is_power_on) {
        // Power-On: RTC-RAM ist leer/uninitialisiert → IMMER aus NVS laden
        uint32_t max_index = 0;
        uint32_t max_pulse = find_max_pulse_and_index_from_nvs(&max_index);
        
        if (max_pulse > 0) {
            // Ring-Speicher enthält Daten → verwende diesen Wert
            *(volatile uint32_t *)&ulp_pulse_counter = max_pulse;
            ESP_LOGI(TAG, "ulp_pulse_counter aus Ring-Speicher: %lu (Power-On, RTC-RAM war: %lu)", max_pulse, current_pulse);
        } else {
            // Ring-Speicher ist leer (z.B. nach Code-Upload mit Versionsmismatch)
            *(volatile uint32_t *)&ulp_pulse_counter = 0;
            ESP_LOGI(TAG, "ulp_pulse_counter auf 0 initialisiert (Power-On, keine Ring-Speicher-Daten, RTC-RAM war: %lu)", current_pulse);
        }
    } else {
        // Deep-Sleep-Wake-up: RTC-RAM ist noch vorhanden → aus RTC-RAM verwenden
        ESP_LOGI(TAG, "ulp_pulse_counter aus RTC-RAM: %lu (RTC-RAM behält Daten bei Deep-Sleep-Wake-up)", current_pulse);
    }
}

// ============================================
// Ressourcen sauber beenden (Web-Server, WiFi, LittleFS)
// ============================================
void shutdown_resources(bool for_imminent_restart) {
    const bool zigbee_was_active =
        (strcmp(config_rtc.transfer_mode, TRANSFER_MODE_ZIGBEE) == 0);

    // ZigBee: CAN_SLEEP abwarten (Main-Loop laeuft), dann Deinit, danach WiFi-Stop
    if (for_imminent_restart && zigbee_was_active && transfer_zigbee_is_initialized()) {
        ESP_LOGI(TAG, "Warte auf ZigBee CAN_SLEEP vor Shutdown (max %d ms)...",
                 ZIGBEE_CAN_SLEEP_WAIT_MS);
        if (transfer_zigbee_wait_can_sleep(ZIGBEE_CAN_SLEEP_WAIT_MS)) {
            ESP_LOGI(TAG, "ZigBee CAN_SLEEP empfangen – Deinit, danach WiFi-Stop");
        } else {
            ESP_LOGW(TAG, "ZigBee CAN_SLEEP Timeout – Deinit und WiFi-Stop trotzdem");
        }
    }

    // Transfer-Modus deinitialisieren (ZigBee Main Loop + NVS-Flush)
    transfer_deinit();
    
    // LED ausschalten (HP-Core wird beendet)
    gpio_set_level((gpio_num_t)LED_BUILTIN_GPIO, LED_OFF);
    ESP_LOGI(TAG, "Interne LED ausgeschaltet");
    
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
    
    // 3. WiFi trennen und stoppen (nach ZigBee-Deinit; Stack/Main-Loop bereits beendet)
    if (wifi_manager_is_initialized()) {
        wifi_manager_session_end();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    // 5. LittleFS unmounten (sichert alle ausstehenden Schreibvorgänge)
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
    
    // WICHTIG: ulp_pulse_counter vor esp_restart() in Ring-Speicher speichern
    // (RTC-RAM wird bei esp_restart() zurückgesetzt)
    ESP_LOGI(TAG, "Speichere ulp_pulse_counter in Ring-Speicher vor Reboot...");
    write_ulp_pulse_counter_to_ring_buffer();
    
    // Ressourcen sauber beenden (ZigBee-NVS-Flush in transfer_zigbee_deinit)
    shutdown_resources(true);
    
    ESP_LOGI(TAG, "Starte Reboot...");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

// ============================================
// Berechne nächsten Timer-Wake-up-Zeitpunkt (Cron-ähnlich)
// ============================================
// Wake-up < 0.5 × wakeup_minutes → diesen Timer überspringen, nächstes volles Intervall
// (Extremfall: Schlafdauer ≈ 0.5× + 1× Intervall). Schützt vor Doppel-Wake nach Time-Sync.
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
    
    // 5. Wake-Interval = nächste Grenze minus Ist-Zeit
    int seconds_until_wakeup = (int)(next_wakeup_time - current_time);

    // 6. Zu kurzer Timer (< 0.5 × Intervall) → ein volles Intervall weiter (Cron-Grenze)
    const int min_sleep_seconds = (interval_seconds + 1) / 2;  // 0.5 × Intervall, aufgerundet
    if (seconds_until_wakeup < min_sleep_seconds) {
        const int skipped_seconds = seconds_until_wakeup;
        next_wakeup_time += interval_seconds;
        seconds_until_wakeup = (int)(next_wakeup_time - current_time);
        ESP_LOGI(TAG, "Wake-up zu kurz (%d s < 0.5×%d min) → übersprungen, nächstes volles Intervall (in %d s)",
                 skipped_seconds, config_rtc.wakeup_minutes, seconds_until_wakeup);
    }

    // 7. Zur Textausgabe: Target-UNIX_EPOCH in Time-struct wandeln
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
    
    // Ressourcen sauber beenden (nur wenn Wake-up konfiguriert wurde)
    // LED wird hier zentral in shutdown_resources() ausgeschaltet
    shutdown_resources(true);
    
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
        bool rtc_valid = true;

        // Wake-up Intervall plausibilisieren
        if (config_rtc.wakeup_minutes < 1 || config_rtc.wakeup_minutes > 60) {
            rtc_valid = false;
        }

        // WiFi-Creds Anzahl plausibilisieren
        if (config_rtc.wifi_count > 2) {
            rtc_valid = false;
        }

        // Transfer-Intervall plausibilisieren
        if (!(config_rtc.transfer_minutes == 255 || config_rtc.transfer_minutes <= 60)) {
            rtc_valid = false;
        }

        // Transfer-Mode plausibilisieren (sicher gegen fehlende Null-Terminierung)
        bool transfer_mode_null_terminated = false;
        for (size_t i = 0; i < sizeof(config_rtc.transfer_mode); i++) {
            if (config_rtc.transfer_mode[i] == '\0') {
                transfer_mode_null_terminated = true;
                break;
            }
        }
        if (!transfer_mode_null_terminated) {
            rtc_valid = false;
        } else {
            if (strcmp(config_rtc.transfer_mode, "zigbee") != 0 &&
                strcmp(config_rtc.transfer_mode, "ble") != 0 &&
                strcmp(config_rtc.transfer_mode, "mqtt") != 0 &&
                strcmp(config_rtc.transfer_mode, "none") != 0) {
                rtc_valid = false;
            }
        }

        // MQTT-Konfiguration plausibilisieren
        if (config_rtc.mqtt_port == 0) {
            rtc_valid = false;
        }
        if (strnlen(config_rtc.mqtt_main_topic, sizeof(config_rtc.mqtt_main_topic)) == 0) {
            rtc_valid = false;
        }

        // RTC-RAM Plausibilität (vor allem TX-Power, damit Brownout/seltene RAM-Korruption
        // nicht zu ungültigen Funktionsparametern führt)
        if (!(config_rtc.wifi_tx_power_dbm == 2 || config_rtc.wifi_tx_power_dbm == 5 ||
              config_rtc.wifi_tx_power_dbm == 8 || config_rtc.wifi_tx_power_dbm == 11 ||
              config_rtc.wifi_tx_power_dbm == 14 || config_rtc.wifi_tx_power_dbm == 17 ||
              config_rtc.wifi_tx_power_dbm == 20)) {
            ESP_LOGW(TAG, "RTC wifi_tx_power_dbm ungültig (%d) → Default", config_rtc.wifi_tx_power_dbm);
            config_rtc.wifi_tx_power_dbm = (int8_t)(WIFI_TX_POWER_DEFAULT / 4);
        }
        if (!(config_rtc.ble_tx_power_dbm == 3 || config_rtc.ble_tx_power_dbm == 6 ||
              config_rtc.ble_tx_power_dbm == 9 || config_rtc.ble_tx_power_dbm == 12 ||
              config_rtc.ble_tx_power_dbm == 15 || config_rtc.ble_tx_power_dbm == 18 ||
              config_rtc.ble_tx_power_dbm == 20)) {
            ESP_LOGW(TAG, "RTC ble_tx_power_dbm ungültig (%d) → Default", config_rtc.ble_tx_power_dbm);
            config_rtc.ble_tx_power_dbm = (int8_t)BLE_TX_POWER_DBM;
        }
        if (!(config_rtc.zigbee_tx_power_dbm == -9 || config_rtc.zigbee_tx_power_dbm == -6 ||
              config_rtc.zigbee_tx_power_dbm == -3 || config_rtc.zigbee_tx_power_dbm == 0 ||
              config_rtc.zigbee_tx_power_dbm == 3 || config_rtc.zigbee_tx_power_dbm == 6 ||
              config_rtc.zigbee_tx_power_dbm == 10)) {
            ESP_LOGW(TAG, "RTC zigbee_tx_power_dbm ungültig (%d) → Default", config_rtc.zigbee_tx_power_dbm);
            config_rtc.zigbee_tx_power_dbm = (int8_t)ZIGBEE_TX_POWER_DEFAULT;
        }

        if (!rtc_valid) {
            ESP_LOGW(TAG, "RTC-Config unplausibel → config.json nachladen");
            config_rtc.config_loaded = false;  // -> normaler Pfad lädt aus Datei
        } else {
            ESP_LOGI(TAG, "Config bereits geladen (aus RTC-RAM) → kein erneutes Laden nötig");
            return true;
        }
    }
    
    if (!mount_littlefs()) {
        return false;
    }
    
    FILE* configFile = fopen("/littlefs/config.json", "r");
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
    if (strlen(hostname) > HOSTNAME_MAX_LEN) {
        ESP_LOGW(TAG, "Hostname '%s' zu lang (%d > %d), wird gekürzt", hostname, strlen(hostname), HOSTNAME_MAX_LEN);
    }
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
    
    // Transfer Intervall: aus config.json oder Default
    // In config.json wird der Multiplikator gespeichert, intern wird es in Minuten umgerechnet
    if (doc["transfer_interval_x"].is<uint8_t>()) {
        uint8_t multiplier = doc["transfer_interval_x"].as<uint8_t>();
        if (multiplier == 0) {
            config_rtc.transfer_minutes = 255;  // Nie
        } else {
            config_rtc.transfer_minutes = multiplier * config_rtc.wakeup_minutes;
        }
    } else if (doc["transfer_minutes"].is<uint8_t>()) {  // Rückwärtskompatibilität: alte config.json mit Minuten (als Multiplikator)
        uint8_t multiplier = doc["transfer_minutes"].as<uint8_t>();
        if (multiplier == 0) {
            config_rtc.transfer_minutes = 255;  // Nie
        } else {
            config_rtc.transfer_minutes = multiplier * config_rtc.wakeup_minutes;
        }
    } else if (doc["tarnsfer_minutes"].is<uint8_t>()) {  // Rückwärtskompatibilität: Tippfehler-Variante
        uint8_t multiplier = doc["tarnsfer_minutes"].as<uint8_t>();
        if (multiplier == 0) {
            config_rtc.transfer_minutes = 255;  // Nie
        } else {
            config_rtc.transfer_minutes = multiplier * config_rtc.wakeup_minutes;
        }
    } else {
        // Default: DEFAULT_TRANSFER_INTERVAL_X * wakeup_minutes (wenn wakeup_minutes aus config.json geladen wurde)
        // oder DEFAULT_TRANSFER_INTERVAL_X * DEFAULT_WAKEUP_INTERVAL_MIN (wenn wakeup_minutes auch nicht gesetzt)
        if (config_rtc.wakeup_minutes > 0) {
            config_rtc.transfer_minutes = DEFAULT_TRANSFER_INTERVAL_X * config_rtc.wakeup_minutes;
        } else {
            config_rtc.transfer_minutes = DEFAULT_TRANSFER_INTERVAL_X * DEFAULT_WAKEUP_INTERVAL_MIN;
        }
    }
    
    // ADC-Multiplikator: aus config.json oder Default aus hardware.h
    if (doc["adc_voltage_multiplier"].is<float>()) {
        config_rtc.adc_voltage_multiplier = doc["adc_voltage_multiplier"].as<float>();
    } else if (doc["adc_voltage_offset"].is<float>()) {
        // Legacy-Fallback: alte Offset-Config näherungsweise auf Multiplikator abbilden.
        // Arbeitspunkt: 4.0 V Akkuspannung (typischer LiPo-Bereich).
        const float legacy_offset = doc["adc_voltage_offset"].as<float>();
        config_rtc.adc_voltage_multiplier = 1.0f + (legacy_offset / 4.0f);
        ESP_LOGW(TAG, "Legacy-Feld adc_voltage_offset erkannt (%.3f V) -> adc_voltage_multiplier=%.4f (nahezu)",
                 legacy_offset, config_rtc.adc_voltage_multiplier);
    } else {
        config_rtc.adc_voltage_multiplier = ADC_VOLTAGE_MULTIPLIER;
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

    if (doc["mqtt_host"].is<const char*>()) {
        const char* mqtt_host = doc["mqtt_host"] | "";
        if (strlen(mqtt_host) > 0) {
            strncpy(config_rtc.mqtt_host, mqtt_host, sizeof(config_rtc.mqtt_host) - 1);
            config_rtc.mqtt_host[sizeof(config_rtc.mqtt_host) - 1] = '\0';
        } else {
            strncpy(config_rtc.mqtt_host, MQTT_DUMMY_HOST, sizeof(config_rtc.mqtt_host) - 1);
            config_rtc.mqtt_host[sizeof(config_rtc.mqtt_host) - 1] = '\0';
        }
    } else {
        strncpy(config_rtc.mqtt_host, MQTT_DUMMY_HOST, sizeof(config_rtc.mqtt_host) - 1);
        config_rtc.mqtt_host[sizeof(config_rtc.mqtt_host) - 1] = '\0';
    }

    if (doc["mqtt_port"].is<uint16_t>()) {
        uint16_t mqtt_port = doc["mqtt_port"].as<uint16_t>();
        config_rtc.mqtt_port = (mqtt_port == 0) ? MQTT_DEFAULT_PORT : mqtt_port;
    } else {
        config_rtc.mqtt_port = MQTT_DEFAULT_PORT;
    }

    if (doc["mqtt_username"].is<const char*>()) {
        const char* mqtt_username = doc["mqtt_username"] | "";
        strncpy(config_rtc.mqtt_username, mqtt_username, sizeof(config_rtc.mqtt_username) - 1);
        config_rtc.mqtt_username[sizeof(config_rtc.mqtt_username) - 1] = '\0';
    } else {
        config_rtc.mqtt_username[0] = '\0';
    }

    if (doc["mqtt_password"].is<const char*>()) {
        const char* mqtt_password = doc["mqtt_password"] | "";
        strncpy(config_rtc.mqtt_password, mqtt_password, sizeof(config_rtc.mqtt_password) - 1);
        config_rtc.mqtt_password[sizeof(config_rtc.mqtt_password) - 1] = '\0';
    } else {
        config_rtc.mqtt_password[0] = '\0';
    }

    if (doc["mqtt_main_topic"].is<const char*>()) {
        const char* mqtt_main_topic = doc["mqtt_main_topic"] | "";
        if (strlen(mqtt_main_topic) > 0) {
            strncpy(config_rtc.mqtt_main_topic, mqtt_main_topic, sizeof(config_rtc.mqtt_main_topic) - 1);
            config_rtc.mqtt_main_topic[sizeof(config_rtc.mqtt_main_topic) - 1] = '\0';
        } else {
            strncpy(config_rtc.mqtt_main_topic, config_rtc.hostname, sizeof(config_rtc.mqtt_main_topic) - 1);
            config_rtc.mqtt_main_topic[sizeof(config_rtc.mqtt_main_topic) - 1] = '\0';
        }
    } else {
        strncpy(config_rtc.mqtt_main_topic, config_rtc.hostname, sizeof(config_rtc.mqtt_main_topic) - 1);
        config_rtc.mqtt_main_topic[sizeof(config_rtc.mqtt_main_topic) - 1] = '\0';
    }

    config_rtc.mqtt_ha_autodiscovery = doc["mqtt_ha_autodiscovery"] | false;
    
    // Transfer-Mode: aus config.json oder Default "none"
    if (doc["transfer_mode"].is<const char*>()) {
        const char* transfer_mode = doc["transfer_mode"].as<const char*>();
        // Validiere Transfer-Mode (nur gültige Werte erlauben)
        if (strcmp(transfer_mode, "zigbee") == 0 || 
            strcmp(transfer_mode, "ble") == 0 || 
            strcmp(transfer_mode, "mqtt") == 0 || 
            strcmp(transfer_mode, "none") == 0) {
            strncpy(config_rtc.transfer_mode, transfer_mode, sizeof(config_rtc.transfer_mode) - 1);
            config_rtc.transfer_mode[sizeof(config_rtc.transfer_mode) - 1] = '\0';
        } else {
            // Ungültiger Wert → Default "none"
            strncpy(config_rtc.transfer_mode, "none", sizeof(config_rtc.transfer_mode) - 1);
            config_rtc.transfer_mode[sizeof(config_rtc.transfer_mode) - 1] = '\0';
        }
    } else {
        // Kein Transfer-Mode in JSON → Default "none"
        strncpy(config_rtc.transfer_mode, "none", sizeof(config_rtc.transfer_mode) - 1);
        config_rtc.transfer_mode[sizeof(config_rtc.transfer_mode) - 1] = '\0';
    }

    // WiFi TX Power (dBm, UI-Stufen)
    if (doc["wifi_tx_power_dbm"].is<int>()) {
        int v = doc["wifi_tx_power_dbm"].as<int>();
        bool ok = (v == 2 || v == 5 || v == 8 || v == 11 || v == 14 || v == 17 || v == 20);
        if (ok) {
            config_rtc.wifi_tx_power_dbm = (int8_t)v;
        } else {
            ESP_LOGW(TAG, "wifi_tx_power_dbm ungültig (%d) → Default", v);
            config_rtc.wifi_tx_power_dbm = (int8_t)(WIFI_TX_POWER_DEFAULT / 4);
        }
    }

    // BLE TX Power (dBm, Stufen)
    if (doc["ble_tx_power_dbm"].is<int>()) {
        int v = doc["ble_tx_power_dbm"].as<int>();
        bool ok = (v == 3 || v == 6 || v == 9 || v == 12 || v == 15 || v == 18 || v == 20);
        if (ok) {
            config_rtc.ble_tx_power_dbm = (int8_t)v;
        } else {
            ESP_LOGW(TAG, "ble_tx_power_dbm ungültig (%d) → Default", v);
            config_rtc.ble_tx_power_dbm = (int8_t)BLE_TX_POWER_DBM;
        }
    }

    // ZigBee TX Power (dBm, Stufen)
    if (doc["zigbee_tx_power_dbm"].is<int>()) {
        int v = doc["zigbee_tx_power_dbm"].as<int>();
        bool ok = (v == -9 || v == -6 || v == -3 || v == 0 || v == 3 || v == 6 || v == 10);
        if (ok) {
            config_rtc.zigbee_tx_power_dbm = (int8_t)v;
        } else {
            ESP_LOGW(TAG, "zigbee_tx_power_dbm ungültig (%d) → Default", v);
            config_rtc.zigbee_tx_power_dbm = (int8_t)ZIGBEE_TX_POWER_DEFAULT;
        }
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
    
    // Debug: Geladene Config (inkl. TX-Power) ausgeben (ohne Passwörter im Klartext)
    ESP_LOGI(TAG, "=== Geladene Config (runtime) ===");
    ESP_LOGI(TAG, "  hostname: %s", config_rtc.hostname);
    ESP_LOGI(TAG, "  adminpass: (Länge: %zu)", strlen(config_rtc.adminpass));
    ESP_LOGI(TAG, "  wakeup_minutes: %d", config_rtc.wakeup_minutes);
    // JSON-Feld transfer_minutes / transfer_interval_x = Multiplikator (x Wake-ups); RTC = effektive Minuten
    if (config_rtc.transfer_minutes == 255) {
        ESP_LOGI(TAG, "  transfer: nie (Multiplikator 0, intern 255)");
    } else if (config_rtc.wakeup_minutes > 0) {
        uint8_t mult = (uint8_t)(config_rtc.transfer_minutes / config_rtc.wakeup_minutes);
        ESP_LOGI(TAG, "  transfer: Multiplikator %u × Wake-up %u Min. = %u Min. effektiv",
                 (unsigned)mult, (unsigned)config_rtc.wakeup_minutes, (unsigned)config_rtc.transfer_minutes);
    } else {
        ESP_LOGI(TAG, "  transfer: %u Min. effektiv (Wake-up ungültig, Multiplikator nicht angezeigt)",
                 (unsigned)config_rtc.transfer_minutes);
    }
    ESP_LOGI(TAG, "  transfer_mode: %s", config_rtc.transfer_mode);
    ESP_LOGI(TAG, "  wifi_tx_power_dbm: %d", config_rtc.wifi_tx_power_dbm);
    ESP_LOGI(TAG, "  ble_tx_power_dbm: %d", config_rtc.ble_tx_power_dbm);
    ESP_LOGI(TAG, "  zigbee_tx_power_dbm: %d", config_rtc.zigbee_tx_power_dbm);
    ESP_LOGI(TAG, "  adc_voltage_multiplier: %.4f", config_rtc.adc_voltage_multiplier);
    ESP_LOGI(TAG, "  ntp_server: %s", config_rtc.ntp_server);
    ESP_LOGI(TAG, "  mqtt_host: %s", config_rtc.mqtt_host);
    ESP_LOGI(TAG, "  mqtt_port: %u", (unsigned int)config_rtc.mqtt_port);
    ESP_LOGI(TAG, "  mqtt_username: %s", config_rtc.mqtt_username);
    ESP_LOGI(TAG, "  mqtt_password: %s (Länge: %zu)",
             (strlen(config_rtc.mqtt_password) > 0 ? "***" : "(leer)"),
             strlen(config_rtc.mqtt_password));
    ESP_LOGI(TAG, "  mqtt_main_topic: %s", config_rtc.mqtt_main_topic);
    ESP_LOGI(TAG, "  mqtt_ha_autodiscovery: %s", config_rtc.mqtt_ha_autodiscovery ? "true" : "false");
    ESP_LOGI(TAG, "  wifiCredentials: %d Set(s)", config_rtc.wifi_count);
    for (uint8_t i = 0; i < config_rtc.wifi_count && i < 2; i++) {
        ESP_LOGI(TAG, "    [%d] SSID: %s", i, config_rtc.wifi_credentials[i].ssid);
        ESP_LOGI(TAG, "    [%d] Password: %s (Länge: %zu)",
                 i,
                 (strlen(config_rtc.wifi_credentials[i].password) > 0 ? "***" : "(leer)"),
                 strlen(config_rtc.wifi_credentials[i].password));
    }
    ESP_LOGI(TAG, "================================");
    
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
        uint8_t multiplier = doc["transfer_minutes"].as<uint8_t>();
        // Multiplikator 0 = nie (keine automatische Übertragung)
        // Multiplikator 1-60: muss sicherstellen, dass multiplier * wakeup_minutes <= 60
        if (multiplier == 0) {
            // 0 = nie: transfer_minutes auf einen sehr großen Wert setzen, damit die Bedingung nie erfüllt wird
            config_rtc.transfer_minutes = 255;  // Maximaler uint8_t Wert, wird nie durch tm_min (0-59) teilbar sein
        } else if (multiplier >= 1 && multiplier <= 60) {
            uint16_t calculated_minutes = multiplier * config_rtc.wakeup_minutes;
            if (calculated_minutes > 60) {
                ESP_LOGE(TAG, "FEHLER: Transfer Intervall ungültig (%d * %d = %d Min., muss <= 60 sein)", 
                         multiplier, config_rtc.wakeup_minutes, calculated_minutes);
                if (errorMessage != nullptr) {
                    char msg[255];
                    snprintf(msg, sizeof(msg), "Fehler: Transfer Intervall ungültig (%d * %d = %d Min., muss <= 60 sein)", 
                             multiplier, config_rtc.wakeup_minutes, calculated_minutes);
                    strncpy(errorMessage, msg, 255);
                }
                return false;
            }
            config_rtc.transfer_minutes = (uint8_t)calculated_minutes;
        } else {
            ESP_LOGE(TAG, "FEHLER: Transfer Intervall Multiplikator ungültig (%d, muss zwischen 0-60 sein)", multiplier);
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: Transfer Intervall Multiplikator ungültig (muss zwischen 0-60 sein, 0 = nie)", 255);
            }
            return false;
        }
    }
    
    if (doc["transfer_mode"].is<const char*>()) {
        const char* transfer_mode = doc["transfer_mode"].as<const char*>();
        if (strcmp(transfer_mode, "zigbee") != 0 && 
            strcmp(transfer_mode, "ble") != 0 && 
            strcmp(transfer_mode, "mqtt") != 0 && 
            strcmp(transfer_mode, "none") != 0) {
            ESP_LOGE(TAG, "FEHLER: Transfer-Mode ungültig (%s, muss zigbee/ble/mqtt/none sein)", transfer_mode);
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: Transfer-Mode ungültig (muss zigbee/ble/mqtt/none sein)", 255);
            }
            return false;
        }
        // Transfer-Mode ist gültig → in config_rtc speichern
        strncpy(config_rtc.transfer_mode, transfer_mode, sizeof(config_rtc.transfer_mode) - 1);
        config_rtc.transfer_mode[sizeof(config_rtc.transfer_mode) - 1] = '\0';
    }

    // WiFi TX Power (dBm, UI-Stufen)
    if (doc["wifi_tx_power_dbm"].is<int>()) {
        int v = doc["wifi_tx_power_dbm"].as<int>();
        bool ok = (v == 2 || v == 5 || v == 8 || v == 11 || v == 14 || v == 17 || v == 20);
        if (ok) {
            config_rtc.wifi_tx_power_dbm = (int8_t)v;
        } else {
            ESP_LOGE(TAG, "FEHLER: wifi_tx_power_dbm ungültig (%d)", v);
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: wifi_tx_power_dbm ungültig", 255);
            }
            return false;
        }
    }

    // BLE TX Power (dBm, Stufen)
    if (doc["ble_tx_power_dbm"].is<int>()) {
        int v = doc["ble_tx_power_dbm"].as<int>();
        bool ok = (v == 3 || v == 6 || v == 9 || v == 12 || v == 15 || v == 18 || v == 20);
        if (ok) {
            config_rtc.ble_tx_power_dbm = (int8_t)v;
        } else {
            ESP_LOGE(TAG, "FEHLER: ble_tx_power_dbm ungültig (%d)", v);
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: ble_tx_power_dbm ungültig", 255);
            }
            return false;
        }
    }

    // ZigBee TX Power (dBm, Stufen)
    if (doc["zigbee_tx_power_dbm"].is<int>()) {
        int v = doc["zigbee_tx_power_dbm"].as<int>();
        bool ok = (v == -9 || v == -6 || v == -3 || v == 0 || v == 3 || v == 6 || v == 10);
        if (ok) {
            config_rtc.zigbee_tx_power_dbm = (int8_t)v;
        } else {
            ESP_LOGE(TAG, "FEHLER: zigbee_tx_power_dbm ungültig (%d)", v);
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: zigbee_tx_power_dbm ungültig", 255);
            }
            return false;
        }
    }
    
    if (doc["adc_voltage_multiplier"].is<float>()) {
        float adc_voltage_multiplier = doc["adc_voltage_multiplier"].as<float>();
        if (adc_voltage_multiplier >= 0.5f && adc_voltage_multiplier <= 2.0f) {
            config_rtc.adc_voltage_multiplier = adc_voltage_multiplier;
        } else {
            ESP_LOGE(TAG, "FEHLER: ADC Spannungs-Multiplikator ungültig (%.4f, muss zwischen 0.5 und 2.0 sein)", adc_voltage_multiplier);
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: ADC Spannungs-Multiplikator ungültig (muss zwischen 0.5 und 2.0 sein)", 255);
            }
            return false;
        }
    } else if (doc["adc_voltage_offset"].is<float>()) {
        // Legacy-Fallback für ältere Clients/Configs.
        float legacy_offset = doc["adc_voltage_offset"].as<float>();
        config_rtc.adc_voltage_multiplier = 1.0f + (legacy_offset / 4.0f);
        ESP_LOGW(TAG, "Legacy-Feld adc_voltage_offset beim Speichern erkannt (%.3f V) -> adc_voltage_multiplier=%.4f",
                 legacy_offset, config_rtc.adc_voltage_multiplier);
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

    if (doc["mqtt_host"].is<const char*>()) {
        const char* mqtt_host = doc["mqtt_host"];
        if (strlen(mqtt_host) < sizeof(config_rtc.mqtt_host)) {
            if (strlen(mqtt_host) == 0) {
                mqtt_host = MQTT_DUMMY_HOST;
            }
            strncpy(config_rtc.mqtt_host, mqtt_host, sizeof(config_rtc.mqtt_host) - 1);
            config_rtc.mqtt_host[sizeof(config_rtc.mqtt_host) - 1] = '\0';
        } else {
            ESP_LOGE(TAG, "FEHLER: MQTT Host zu lang");
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: MQTT Host zu lang", 255);
            }
            return false;
        }
    }

    if (doc["mqtt_port"].is<uint16_t>()) {
        uint16_t mqtt_port = doc["mqtt_port"].as<uint16_t>();
        if (mqtt_port >= 1) {
            config_rtc.mqtt_port = mqtt_port;
        } else {
            ESP_LOGE(TAG, "FEHLER: MQTT Port ungültig");
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: MQTT Port ungültig", 255);
            }
            return false;
        }
    }

    if (doc["mqtt_username"].is<const char*>()) {
        const char* mqtt_username = doc["mqtt_username"];
        if (strlen(mqtt_username) < sizeof(config_rtc.mqtt_username)) {
            strncpy(config_rtc.mqtt_username, mqtt_username, sizeof(config_rtc.mqtt_username) - 1);
            config_rtc.mqtt_username[sizeof(config_rtc.mqtt_username) - 1] = '\0';
        } else {
            ESP_LOGE(TAG, "FEHLER: MQTT Username zu lang");
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: MQTT Username zu lang", 255);
            }
            return false;
        }
    }

    if (doc["mqtt_password"].is<const char*>()) {
        const char* mqtt_password = doc["mqtt_password"];
        if (strlen(mqtt_password) < sizeof(config_rtc.mqtt_password)) {
            strncpy(config_rtc.mqtt_password, mqtt_password, sizeof(config_rtc.mqtt_password) - 1);
            config_rtc.mqtt_password[sizeof(config_rtc.mqtt_password) - 1] = '\0';
        } else {
            ESP_LOGE(TAG, "FEHLER: MQTT Passwort zu lang");
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: MQTT Passwort zu lang", 255);
            }
            return false;
        }
    }

    if (doc["mqtt_main_topic"].is<const char*>()) {
        const char* mqtt_main_topic = doc["mqtt_main_topic"];
        if (strlen(mqtt_main_topic) < sizeof(config_rtc.mqtt_main_topic)) {
            strncpy(config_rtc.mqtt_main_topic, mqtt_main_topic, sizeof(config_rtc.mqtt_main_topic) - 1);
            config_rtc.mqtt_main_topic[sizeof(config_rtc.mqtt_main_topic) - 1] = '\0';
        } else {
            ESP_LOGE(TAG, "FEHLER: MQTT Main Topic zu lang");
            if (errorMessage != nullptr) {
                strncpy(errorMessage, "Fehler: MQTT Main Topic zu lang", 255);
            }
            return false;
        }
    }

    config_rtc.mqtt_ha_autodiscovery = doc["mqtt_ha_autodiscovery"] | false;

    if (strcmp(config_rtc.transfer_mode, TRANSFER_MODE_MQTT) == 0) {
        if (strlen(config_rtc.mqtt_host) == 0) {
            strncpy(config_rtc.mqtt_host, MQTT_DUMMY_HOST, sizeof(config_rtc.mqtt_host) - 1);
            config_rtc.mqtt_host[sizeof(config_rtc.mqtt_host) - 1] = '\0';
        }
        if (strlen(config_rtc.mqtt_main_topic) == 0) {
            strncpy(config_rtc.mqtt_main_topic, config_rtc.hostname, sizeof(config_rtc.mqtt_main_topic) - 1);
            config_rtc.mqtt_main_topic[sizeof(config_rtc.mqtt_main_topic) - 1] = '\0';
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
    FILE* configFile = fopen("/littlefs/config.json", "w");
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
    // Multiplikator berechnen und speichern
    // Wenn transfer_minutes == 255, dann ist Multiplikator = 0 (nie)
    uint8_t multiplier;
    if (config_rtc.transfer_minutes == 255) {
        multiplier = 0;  // Nie
    } else if (config_rtc.wakeup_minutes > 0) {
        multiplier = config_rtc.transfer_minutes / config_rtc.wakeup_minutes;
    } else {
        multiplier = DEFAULT_TRANSFER_INTERVAL_X;
    }
    newDoc["transfer_interval_x"] = multiplier;
    newDoc["transfer_mode"] = config_rtc.transfer_mode;
    newDoc["wifi_tx_power_dbm"] = (int)config_rtc.wifi_tx_power_dbm;
    newDoc["ble_tx_power_dbm"] = (int)config_rtc.ble_tx_power_dbm;
    newDoc["zigbee_tx_power_dbm"] = (int)config_rtc.zigbee_tx_power_dbm;
    newDoc["adc_voltage_multiplier"] = config_rtc.adc_voltage_multiplier;
    newDoc["ntp_server"] = config_rtc.ntp_server;
    newDoc["mqtt_host"] = config_rtc.mqtt_host;
    newDoc["mqtt_port"] = config_rtc.mqtt_port;
    newDoc["mqtt_username"] = config_rtc.mqtt_username;
    newDoc["mqtt_password"] = config_rtc.mqtt_password;
    newDoc["mqtt_main_topic"] = config_rtc.mqtt_main_topic;
    newDoc["mqtt_ha_autodiscovery"] = config_rtc.mqtt_ha_autodiscovery;
    
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

    ESP_LOGI(TAG, "MQTT Config gespeichert: host=%s port=%u user=%s password=%s topic=%s ha_auto=%s",
             config_rtc.mqtt_host,
             (unsigned int)config_rtc.mqtt_port,
             config_rtc.mqtt_username,
             (strlen(config_rtc.mqtt_password) > 0 ? "***" : "(leer)"),
             config_rtc.mqtt_main_topic,
             config_rtc.mqtt_ha_autodiscovery ? "true" : "false");
    
    ESP_LOGI(TAG, "Config erfolgreich gespeichert (RTC-RAM und config.json)");
    return true;
}

// Statische Variable für mDNS-Initialisierung
static bool mdns_initialized = false;

// ============================================
// NTP-Zeitsynchronisation (ESP-IDF)
// ============================================
// Callback für Zeit-Synchronisation
static void time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "Zeit synchronisiert");
}

bool sync_ntp_time() {
    if (!wifi_is_connected()) {
        ESP_LOGE(TAG, "NTP-Sync: WiFi nicht verbunden");
        return false;
    }
    
    ESP_LOGI(TAG, "NTP-Sync: Verbinde mit %s...", config_rtc.ntp_server);
    
    // Zeitzone explizit auf UTC setzen (für Datenlogger wichtig - keine Sommer/Winterzeit-Sprünge!)
    setenv("TZ", "UTC", 1);
    tzset();
    
    // NTP-Konfiguration (ESP-IDF SNTP)
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, config_rtc.ntp_server);
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
        time_sync_set_hard(now, "NTP");
        ESP_LOGI(TAG, "NTP-Sync erfolgreich (UTC): %04d-%02d-%02d %02d:%02d:%02d UTC",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        int64_t sec_since = time_sync_seconds_since_last();
        if (sec_since >= 0) {
            ESP_LOGI(TAG, "  Letzte Sync: vor %lld Sekunden", (long long)sec_since);
        }
        return true;
    } else {
        ESP_LOGE(TAG, "NTP-Sync fehlgeschlagen (Timeout)");
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
    if (strcmp(var, "adc_voltage_multiplier") == 0) {
        snprintf(buffer, sizeof(buffer), "%.4f", config_rtc.adc_voltage_multiplier);
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
    if (strcmp(var, "board_version") == 0) {
        snprintf(buffer, sizeof(buffer), "%d", BOARD_VERSION_ID);
        return buffer;
    }
    if (strcmp(var, "currentWifiData") == 0) {
        // Aktuelle WiFi-Credentials für Vergleich
        static char json_buffer[256];
        strcpy(json_buffer, "[");
        if (wifi_is_connected()) {
            const wifi_ap_record_t* ap = wifi_get_ap_info();
            // Finde passendes Credential
            for (uint8_t i = 0; i < config_rtc.wifi_count; i++) {
                if (strcmp((const char*)ap->ssid, config_rtc.wifi_credentials[i].ssid) == 0) {
                    strcat(json_buffer, "{");
                    strcat(json_buffer, "\"ssid\":\"");
                    strcat(json_buffer, (const char*)ap->ssid);
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
    if (strcmp(var, "mqtt_host") == 0) {
        return config_rtc.mqtt_host;
    }
    if (strcmp(var, "mqtt_port") == 0) {
        snprintf(buffer, sizeof(buffer), "%u", (unsigned int)config_rtc.mqtt_port);
        return buffer;
    }
    if (strcmp(var, "mqtt_username") == 0) {
        return config_rtc.mqtt_username;
    }
    if (strcmp(var, "mqtt_password") == 0) {
        return config_rtc.mqtt_password;
    }
    if (strcmp(var, "mqtt_main_topic") == 0) {
        return config_rtc.mqtt_main_topic;
    }
    if (strcmp(var, "mqtt_dummy_host_default") == 0) {
        return MQTT_DUMMY_HOST;
    }
    if (strcmp(var, "mqtt_default_port_value") == 0) {
        snprintf(buffer, sizeof(buffer), "%u", (unsigned int)MQTT_DEFAULT_PORT);
        return buffer;
    }
    if (strcmp(var, "mqtt_default_main_topic_value") == 0) {
        /* UI-Vorschlag = Hostname (ZigBee/BLE-Limit 26 ≤ MQTT_TOPIC 63; typ. a-z0-9- → MQTT-Level ok). */
        if (config_rtc.hostname[0] != '\0') {
            return config_rtc.hostname;
        }
        return MQTT_DEFAULT_MAIN_TOPIC;
    }
    if (strcmp(var, "mqtt_ha_autodiscovery_checked") == 0) {
        return config_rtc.mqtt_ha_autodiscovery ? "checked" : "";
    }
    if (strcmp(var, "wifi_tx_power_dbm") == 0) {
        snprintf(buffer, sizeof(buffer), "%d", config_rtc.wifi_tx_power_dbm);
        return buffer;
    }
    if (strcmp(var, "ble_tx_power_dbm") == 0) {
        snprintf(buffer, sizeof(buffer), "%d", config_rtc.ble_tx_power_dbm);
        return buffer;
    }
    if (strcmp(var, "zigbee_tx_power_dbm") == 0) {
        snprintf(buffer, sizeof(buffer), "%d", config_rtc.zigbee_tx_power_dbm);
        return buffer;
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
    if (strcmp(var, "ulp_pulse_counter") == 0) {
        // Formatierung: 7 Stellen mit führenden Nullen (99999.99 = 9999999)
        uint32_t current_pulse = *(volatile uint32_t *)&ulp_pulse_counter;
        snprintf(buffer, sizeof(buffer), "%07lu", current_pulse);
        return buffer;
    }
    if (strcmp(var, "pulse_counter_left") == 0) {
        // Linke 5 Stellen für CSS-Formatierung (Vorkommastellen)
        uint32_t current_pulse = *(volatile uint32_t *)&ulp_pulse_counter;
        snprintf(buffer, sizeof(buffer), "%05lu", current_pulse / PULSE_COUNTER_DIVISOR);
        return buffer;
    }
    if (strcmp(var, "pulse_counter_right") == 0) {
        // Rechte 2 Stellen für CSS-Formatierung (Nachkommastellen)
        uint32_t current_pulse = *(volatile uint32_t *)&ulp_pulse_counter;
        snprintf(buffer, sizeof(buffer), "%02lu", current_pulse % PULSE_COUNTER_DIVISOR);
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
    if (strcmp(var, "time_sync_last") == 0) {
        int64_t sec_since = time_sync_seconds_since_last();
        if (sec_since < 0) {
            snprintf(buffer, sizeof(buffer), "Nie synchronisiert");
            return buffer;
        }
        struct tm timeinfo;
        time_t last_epoch = time_sync_last_epoch;
        if (gmtime_r(&last_epoch, &timeinfo)) {
            char time_buf[48];
            const char *src = (time_sync_last_source[0] != '\0') ? time_sync_last_source : "?";
            snprintf(time_buf, sizeof(time_buf), "%04d-%02d-%02d %02d:%02d:%02d UTC (%s)",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, src);
            if (sec_since < 60) {
                snprintf(buffer, sizeof(buffer), "%s (vor %lld s)", time_buf, (long long)sec_since);
            } else if (sec_since < 3600) {
                snprintf(buffer, sizeof(buffer), "%s (vor %lld Min)", time_buf, (long long)(sec_since / 60));
            } else if (sec_since < 86400) {
                snprintf(buffer, sizeof(buffer), "%s (vor %lld Std)", time_buf, (long long)(sec_since / 3600));
            } else {
                snprintf(buffer, sizeof(buffer), "%s (vor %lld Tagen)", time_buf, (long long)(sec_since / 86400));
            }
            return buffer;
        }
        snprintf(buffer, sizeof(buffer), "vor %lld s", (long long)sec_since);
        return buffer;
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
    if (strcmp(var, "transfer_mode") == 0) {
        snprintf(buffer, sizeof(buffer), "%s", config_rtc.transfer_mode);
        return buffer;
    }
    if (strcmp(var, "transfer_mode_none") == 0) {
        return (strcmp(config_rtc.transfer_mode, "none") == 0) ? "selected" : "";
    }
    if (strcmp(var, "transfer_mode_zigbee") == 0) {
        return (strcmp(config_rtc.transfer_mode, "zigbee") == 0) ? "selected" : "";
    }
    if (strcmp(var, "transfer_mode_ble") == 0) {
        return (strcmp(config_rtc.transfer_mode, "ble") == 0) ? "selected" : "";
    }
    if (strcmp(var, "transfer_mode_mqtt") == 0) {
        return (strcmp(config_rtc.transfer_mode, "mqtt") == 0) ? "selected" : "";
    }
    if (strcmp(var, "ble_status") == 0) {
        return "Nicht aktiv";
    }
    if (strcmp(var, "ble_mac") == 0) {
        uint8_t mac[6] = {0};
        if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
            snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
            snprintf(buffer, sizeof(buffer), "-");
        }
        return buffer;
    }
    if (strcmp(var, "transfer_interval_x") == 0) {
        // Wenn transfer_minutes == 255, dann ist Multiplikator = 0 (nie)
        if (config_rtc.transfer_minutes == 255) {
            snprintf(buffer, sizeof(buffer), "0");
        } else if (config_rtc.wakeup_minutes > 0) {
            uint8_t multiplier = config_rtc.transfer_minutes / config_rtc.wakeup_minutes;
            snprintf(buffer, sizeof(buffer), "%d", multiplier);
        } else {
            // Sollte nicht vorkommen, da Defaults beim Laden gesetzt werden
            snprintf(buffer, sizeof(buffer), "%d", DEFAULT_TRANSFER_INTERVAL_X);
        }
        return buffer;
    }
    if (strcmp(var, "transfer_interval_minutes_display") == 0) {
        // Wenn transfer_minutes == 255, dann ist es "nie"
        if (config_rtc.transfer_minutes == 255) {
            snprintf(buffer, sizeof(buffer), "nie");
        } else {
            snprintf(buffer, sizeof(buffer), "%d", config_rtc.transfer_minutes);
        }
        return buffer;
    }
    if (strcmp(var, "transfer_interval_display") == 0) {
        // Vollständige Anzeige: "nie" oder "2x (20 Min.)"
        // Defaults werden bereits beim Laden der Config gesetzt, daher hier nur Anzeige
        if (config_rtc.transfer_minutes == 255) {
            snprintf(buffer, sizeof(buffer), "nie");
        } else if (config_rtc.wakeup_minutes > 0) {
            uint8_t multiplier = config_rtc.transfer_minutes / config_rtc.wakeup_minutes;
            snprintf(buffer, sizeof(buffer), "%dx (%d Min.)", multiplier, config_rtc.transfer_minutes);
        } else {
            // Sollte nicht vorkommen, da Defaults beim Laden gesetzt werden
            uint8_t default_multiplier = DEFAULT_TRANSFER_INTERVAL_X;
            uint8_t default_wakeup = DEFAULT_WAKEUP_INTERVAL_MIN;
            uint8_t default_transfer_minutes = default_multiplier * default_wakeup;
            snprintf(buffer, sizeof(buffer), "%dx (%d Min.)", default_multiplier, default_transfer_minutes);
        }
        return buffer;
    }
    // ZigBee-Status-Variablen
    if (strcmp(var, "zigbee_status") == 0) {
        // Prüfe, ob ZigBee initialisiert ist und Status ermitteln
        bool is_factory_new = false;
        bool is_joined = false;
        
        if (strcmp(config_rtc.transfer_mode, "zigbee") == 0) {
            is_joined = transfer_zigbee_is_joined();
            is_factory_new = transfer_zigbee_is_factory_new();
        }
        
        if (strcmp(config_rtc.transfer_mode, "zigbee") != 0) {
            snprintf(buffer, sizeof(buffer), "Nicht aktiv");
        } else if (is_joined) {
            snprintf(buffer, sizeof(buffer), "Gepaart");
        } else if (is_factory_new) {
            snprintf(buffer, sizeof(buffer), "Factory-New (nicht gepaart)");
        } else {
            snprintf(buffer, sizeof(buffer), "Nicht gepaart");
        }
        return buffer;
    }
    if (strcmp(var, "zigbee_network_addr") == 0) {
        if (strcmp(config_rtc.transfer_mode, "zigbee") == 0 && ZIGBEE_IS_NETWORK_ADDR_VALID(zigbee_rtc.network_addr)) {
            snprintf(buffer, sizeof(buffer), "0x%04X", zigbee_rtc.network_addr);
        } else {
            snprintf(buffer, sizeof(buffer), "-");
        }
        return buffer;
    }
    if (strcmp(var, "zigbee_pan_id") == 0) {
        if (strcmp(config_rtc.transfer_mode, "zigbee") == 0 && zigbee_rtc.pan_id != 0) {
            snprintf(buffer, sizeof(buffer), "0x%04X", zigbee_rtc.pan_id);
        } else {
            snprintf(buffer, sizeof(buffer), "-");
        }
        return buffer;
    }
    if (strcmp(var, "zigbee_channel") == 0) {
        if (strcmp(config_rtc.transfer_mode, "zigbee") == 0 && zigbee_rtc.channel != 0) {
            snprintf(buffer, sizeof(buffer), "%d", zigbee_rtc.channel);
        } else {
            snprintf(buffer, sizeof(buffer), "-");
        }
        return buffer;
    }
    if (strcmp(var, "zigbee_extended_addr") == 0) {
        if (strcmp(config_rtc.transfer_mode, "zigbee") == 0 && zigbee_rtc.extended_addr != 0) {
            snprintf(buffer, sizeof(buffer), "0x%016llX", (unsigned long long)zigbee_rtc.extended_addr);
        } else {
            snprintf(buffer, sizeof(buffer), "-");
        }
        return buffer;
    }
    if (strcmp(var, "usb_connected") == 0) {
#if BOARD_VERSION_ID == 20260523
        return IS_USB_POWER(battery_voltage) ? "JA" : "NEIN";
#else
        return "nicht erkennbar";
#endif
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
        return wifi_is_connected() ? "display:block;" : "display:none;";
    }
    if (strcmp(var, "wifi_ip") == 0) {
        if (wifi_is_connected()) {
            snprintf(buffer, sizeof(buffer), IPSTR, IP2STR(&wifi_get_ip_info()->ip));
            return buffer;
        }
        return "-";
    }
    if (strcmp(var, "wifi_rssi") == 0) {
        if (wifi_is_connected()) {
            snprintf(buffer, sizeof(buffer), "%d", wifi_get_ap_info()->rssi);
            return buffer;
        }
        return "-";
    }
    if (strcmp(var, "wifi_ssid") == 0) {
        // Zeige aktuell verbundenes SSID (falls verbunden)
        if (wifi_is_connected()) {
            return (const char*)wifi_get_ap_info()->ssid;
        }
        return "Nicht verbunden";
    }
    if (strcmp(var, "wifi_status") == 0) {
        return wifi_is_connected() ? "Verbunden" : "Nicht verbunden";
    }
    if (strcmp(var, "wifi_sta_connected") == 0) {
        return wifi_is_connected() ? "1" : "0";
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
    struct sockaddr_storage peer = {};
    socklen_t peer_len = sizeof(peer);
    char peer_ip[16] = "?";
    int sock = httpd_req_to_sockfd(req);
    if (sock >= 0 && getpeername(sock, reinterpret_cast<struct sockaddr*>(&peer), &peer_len) == 0 &&
        peer.ss_family == AF_INET) {
        inet_ntoa_r(reinterpret_cast<struct sockaddr_in*>(&peer)->sin_addr, peer_ip, sizeof(peer_ip));
    }
    ESP_LOGI(TAG, "GET /ping von %s", peer_ip);
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
    uint32_t current_pulse = *(volatile uint32_t *)&ulp_pulse_counter;
    uint32_t vorkommastellen = current_pulse / PULSE_COUNTER_DIVISOR;
    uint32_t nachkommastellen = current_pulse % PULSE_COUNTER_DIVISOR;
    
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
    battery_voltage = (float)battery_adc_mv / 1000.0f * VOLTAGE_DIVIDER_RATIO * config_rtc.adc_voltage_multiplier;
    battery_percent = VOLTAGE_TO_PERCENT(battery_voltage);
    
    // Template-Datei verarbeiten
    return process_template_file("/littlefs/index.html", req);
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
    return process_template_file("/littlefs/config.html", req);
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

static const size_t HTTP_ACTION_POST_MAX = 1024;

static esp_err_t http_send_401_unauthorized(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"GasOMeterKonfiguration\"");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
}

static bool http_require_config_auth(httpd_req_t* req) {
    return check_basic_auth(req, "admin", config_rtc.adminpass);
}

static int http_read_post_form(httpd_req_t* req, char* post_data, size_t post_cap) {
    if (req->content_len == 0 || req->content_len >= post_cap) {
        return -1;
    }
    return read_post_data(req, post_data, post_cap);
}

/** true, wenn config_rtc.transfer_mode dem erwarteten Modus entspricht (zigbee/ble/mqtt). */
static bool http_transfer_mode_is(const char* expected_mode) {
    return expected_mode != nullptr && strcmp(config_rtc.transfer_mode, expected_mode) == 0;
}

/** 400 JSON, wenn falscher Modus — verhindert Status/Aktionen eines anderen Stacks (kein Crash, aber irreführende RTC-Daten). */
static esp_err_t http_reject_unless_transfer_mode(httpd_req_t* req, const char* expected_mode,
                                                    const char* label) {
    if (http_transfer_mode_is(expected_mode)) {
        return ESP_OK;
    }
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    char json[128];
    snprintf(json, sizeof(json),
             "{\"error\":\"%s ist nicht der aktive Uebertragungsmodus (gespeichert: %s)\"}", label,
             config_rtc.transfer_mode);
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
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

// Hilfsfunktion: POST-Parameter aus application/x-www-form-urlencoded extrahieren
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
    
    // Parameter extrahieren
    char json_data[1024] = "";
    char current_password[64] = "";
    
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
    
    // current_password Parameter (optional)
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
    if (doc["transfer_mode"].is<const char*>()) {
        ESP_LOGI(TAG, "  transfer_mode: %s", doc["transfer_mode"].as<const char*>());
    }
    if (doc["adc_voltage_multiplier"].is<float>()) {
        ESP_LOGI(TAG, "  adc_voltage_multiplier: %.4f", doc["adc_voltage_multiplier"].as<float>());
    }
    if (doc["adc_voltage_offset"].is<float>()) {
        ESP_LOGI(TAG, "  adc_voltage_offset (legacy): %.3f V", doc["adc_voltage_offset"].as<float>());
    }
    if (doc["ntp_server"].is<const char*>()) {
        ESP_LOGI(TAG, "  ntp_server: %s", doc["ntp_server"].as<const char*>());
    }
    if (doc["wifi_tx_power_dbm"].is<int>()) {
        ESP_LOGI(TAG, "  wifi_tx_power_dbm: %d", doc["wifi_tx_power_dbm"].as<int>());
    }
    if (doc["ble_tx_power_dbm"].is<int>()) {
        ESP_LOGI(TAG, "  ble_tx_power_dbm: %d", doc["ble_tx_power_dbm"].as<int>());
    }
    if (doc["zigbee_tx_power_dbm"].is<int>()) {
        ESP_LOGI(TAG, "  zigbee_tx_power_dbm: %d", doc["zigbee_tx_power_dbm"].as<int>());
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
        
        ESP_LOGI(TAG, "Config erfolgreich gespeichert (config.json)");
        ESP_LOGI(TAG, "  → RTC-Config invalidiert - wird beim nächsten Start aus config.json geladen");
        ESP_LOGI(TAG, "  → TX Power (applied): wifi=%d dBm, ble=%d dBm, zigbee=%d dBm",
                 config_rtc.wifi_tx_power_dbm,
                 config_rtc.ble_tx_power_dbm,
                 config_rtc.zigbee_tx_power_dbm);
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
    const size_t MAX_POST_SIZE = 512;
    size_t content_len = req->content_len;
    
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
    
    // Prüfe, ob ein Factory-Reset gerade läuft
    if (transfer_zigbee_is_factory_reset_in_progress()) {
        ESP_LOGW(TAG, "Reboot-Handler: Reboot abgelehnt - Factory-Reset läuft gerade");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Reboot nicht möglich: ZigBee Factory-Reset läuft gerade\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Reboot wird durchgeführt...", HTTPD_RESP_USE_STRLEN);
    
    // Reboot-Variable setzen (Ausführung in web_timeout_task)
    reboot_requested = true;
    reboot_reason = "Reboot durch Web-Interface ausgelöst";
    
    return ESP_OK;
}

// Handler für /deepsleep (POST)
// WICHTIG: Nur POST-Body wird akzeptiert, KEINE Query-Parameter (Sicherheitsmaßnahme gegen versehentliches Deep-Sleep)
static esp_err_t deepsleep_handler(httpd_req_t *req) {
    const size_t MAX_POST_SIZE = 512;
    size_t content_len = req->content_len;
    
    ESP_LOGD(TAG, "DeepSleep-Handler: Content-Length=%zu", content_len);
    
    if (content_len == 0 || content_len > MAX_POST_SIZE) {
        ESP_LOGE(TAG, "DeepSleep-Handler: POST-Daten ungültig (Content-Length=%zu, Max=%zu)", content_len, MAX_POST_SIZE);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: POST-Daten ungültig", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    char post_data[512];
    int len = read_post_data(req, post_data, sizeof(post_data));
    if (len < 0) {
        ESP_LOGE(TAG, "DeepSleep-Handler: POST-Daten konnten nicht gelesen werden");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: POST-Daten konnten nicht gelesen werden", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // DEBUG: Zeige empfangene POST-Daten
    ESP_LOGD(TAG, "DeepSleep-Handler: POST-Daten empfangen (%d Bytes): %.200s", len, post_data);
    
    // POST-Parameter prüfen: cmd=deepsleep
    char cmd[32] = "";
    if (!get_post_param(post_data, len, "cmd", cmd, sizeof(cmd))) {
        ESP_LOGE(TAG, "DeepSleep-Handler: Parameter 'cmd' nicht gefunden in POST-Daten");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: Parameter 'cmd=deepsleep' erforderlich", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    ESP_LOGD(TAG, "DeepSleep-Handler: cmd='%s'", cmd);
    
    if (strcmp(cmd, "deepsleep") != 0) {
        ESP_LOGE(TAG, "DeepSleep-Handler: cmd='%s' != 'deepsleep'", cmd);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: Parameter 'cmd=deepsleep' erforderlich", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // WICHTIG: Aktivität NACH Validierung aktualisieren, um Race-Condition mit Web-Timeout-Task zu vermeiden
    // Der Timeout-Task läuft asynchron und könnte zwischenzeitlich Deep-Sleep auslösen
    last_web_activity_us = esp_timer_get_time();
    
    // Prüfe, ob ein Factory-Reset gerade läuft
    if (transfer_zigbee_is_factory_reset_in_progress()) {
        ESP_LOGW(TAG, "DeepSleep-Handler: Deep-Sleep abgelehnt - Factory-Reset läuft gerade");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Deep-Sleep nicht möglich: ZigBee Factory-Reset läuft gerade\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Deep-Sleep wird durchgeführt...", HTTPD_RESP_USE_STRLEN);
    
    // Deep-Sleep-Variable setzen (ähnlich wie reboot_requested)
    should_enter_deep_sleep = true;
    deep_sleep_reason = "Deep-Sleep durch Web-Interface ausgelöst";
    
    return ESP_OK;
}

// Handler für /counter/set (POST)
static esp_err_t counter_set_handler(httpd_req_t *req) {
    last_web_activity_us = esp_timer_get_time();
    
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
    
    uint32_t old_value = *(volatile uint32_t *)&ulp_pulse_counter;
    ESP_LOGI(TAG, "Zählerstand manuell gesetzt: %lu → %lu",
             (unsigned long)old_value, (unsigned long)new_value);

    if (!init_pulse_nvs_minimal()) {
        ESP_LOGE(TAG, "counter/set: Pulse-NVS nicht initialisiert");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: Pulse-NVS nicht initialisiert", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // ring_idx sicherstellen (nach Power-On/RTC-Verlust sonst ungültig)
    if (ring_idx >= RING_BUFFER_SIZE) {
        uint32_t max_index = 0;
        uint32_t max_pulse = find_max_pulse_and_index_from_nvs(&max_index);
        ring_idx = (max_pulse > 0) ? ((max_index + 1) % RING_BUFFER_SIZE) : 0;
        ESP_LOGI(TAG, "counter/set: ring_idx neu ermittelt: %lu", (unsigned long)ring_idx);
    }

    // RTC sofort aktualisieren (UI/LP-Core)
    *(volatile uint32_t *)&ulp_pulse_counter = new_value;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open_from_partition(NVS_PARTITION_PULSE, NVS_NAMESPACE_PULSE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "counter/set: NVS-Open fehlgeschlagen: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: Ring-Speicher konnte nicht geöffnet werden", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Korrektur nach unten: höhere Ring-Einträge entfernen, sonst gewinnt find_max den alten Wert
    if (new_value < old_value) {
        uint32_t deleted_count = 0;
        for (uint32_t i = 0; i < RING_BUFFER_SIZE; i++) {
            char key[MAX_KEY_LENGTH];
            snprintf(key, sizeof(key), "%s%lu", NVS_KEY_PREFIX, (unsigned long)i);
            uint32_t pulse_value = 0;
            if (nvs_get_u32(nvs_handle, key, &pulse_value) == ESP_OK && pulse_value > new_value) {
                if (nvs_erase_key(nvs_handle, key) == ESP_OK) {
                    deleted_count++;
                }
            }
        }
        ESP_LOGI(TAG, "counter/set: Ring bereinigt, %lu Einträge > %lu gelöscht",
                 (unsigned long)deleted_count, (unsigned long)new_value);
    }

    // Neuen Wert schreiben (wie write_ulp: an ring_idx, danach Index erhöhen)
    char key[MAX_KEY_LENGTH];
    snprintf(key, sizeof(key), "%s%lu", NVS_KEY_PREFIX, (unsigned long)ring_idx);
    err = nvs_set_u32(nvs_handle, key, new_value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "counter/set: nvs_set_u32(%s=%lu) fehlgeschlagen: %s",
                 key, (unsigned long)new_value, esp_err_to_name(err));
        nvs_close(nvs_handle);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: Zählerstand konnte nicht in NVS geschrieben werden", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "counter/set: nvs_commit fehlgeschlagen: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: NVS-Commit fehlgeschlagen", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    const uint32_t written_at = ring_idx;
    ring_idx = (ring_idx + 1) % RING_BUFFER_SIZE;
    ESP_LOGI(TAG, "counter/set: NVS OK — %s=%lu (nächster ring_idx=%lu)",
             key, (unsigned long)new_value, (unsigned long)ring_idx);

    // Verifikation: denselben Key zurücklesen
    err = nvs_open_from_partition(NVS_PARTITION_PULSE, NVS_NAMESPACE_PULSE, NVS_READONLY, &nvs_handle);
    uint32_t readback = 0;
    esp_err_t read_err = ESP_FAIL;
    if (err == ESP_OK) {
        read_err = nvs_get_u32(nvs_handle, key, &readback);
        nvs_close(nvs_handle);
    }
    ESP_LOGI(TAG, "counter/set: NVS-Verify %s → %s, Wert=%lu (erwartet %lu, Slot %lu)",
             key, esp_err_to_name(read_err), (unsigned long)readback,
             (unsigned long)new_value, (unsigned long)written_at);
    if (read_err != ESP_OK || readback != new_value) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: NVS-Verify fehlgeschlagen (Wert nicht lesbar)", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char response[128];
    snprintf(response, sizeof(response),
             "Zählerstand gesetzt und in NVS gespeichert: %05lu.%02lu",
             (unsigned long)(new_value / PULSE_COUNTER_DIVISOR),
             (unsigned long)(new_value % PULSE_COUNTER_DIVISOR));
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler für /api/pulse_counter (GET) - JSON API für Gas-Counter mit Debug-Info
static esp_err_t pulse_counter_api_handler(httpd_req_t *req) {
    last_web_activity_us = esp_timer_get_time();
    
    // Aktuellen Pulse-Counter-Wert aus RTC-RAM lesen
    uint32_t current_pulse = *(volatile uint32_t *)&ulp_pulse_counter;
    uint32_t lp_core_running = *(volatile uint32_t *)&ulp_lp_core_running;
    
    // JSON-Response erstellen (mit Debug-Informationen)
    char json_response[256];
    snprintf(json_response, sizeof(json_response),
             "{\"pulse_counter\":%lu,\"left\":\"%05lu\",\"right\":\"%02lu\",\"lp_core_running\":%lu,\"divisor\":%d}",
             current_pulse,
             current_pulse / PULSE_COUNTER_DIVISOR,
             current_pulse % PULSE_COUNTER_DIVISOR,
             lp_core_running,
             PULSE_COUNTER_DIVISOR);
    
    // Debug-Log
    ESP_LOGI(TAG, "API: pulse_counter=%lu, left=%05lu, right=%02lu, lp_core_running=%lu",
             current_pulse, current_pulse / PULSE_COUNTER_DIVISOR, current_pulse % PULSE_COUNTER_DIVISOR, lp_core_running);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/** JSON aus get_status_json; optional Long-Poll-Metadaten anhaengen. */
static bool zigbee_status_format_json(char* out, size_t out_size, uint32_t waited_ms, bool include_wait_meta,
                                      bool timed_out) {
    char base[512];
    if (!transfer_zigbee_get_status_json(base, sizeof(base))) {
        return false;
    }
    if (!include_wait_meta) {
        snprintf(out, out_size, "%s", base);
        return true;
    }
    const size_t len = strlen(base);
    if (len < 2 || base[len - 1] != '}') {
        return false;
    }
    base[len - 1] = '\0';
    const int written = snprintf(out, out_size, "%s,\"waited_ms\":%lu,\"timed_out\":%s}", base,
                                 (unsigned long)waited_ms, timed_out ? "true" : "false");
    return written > 0 && (size_t)written < out_size;
}

// Handler für /zigbee/status (GET); optional ?wait=N Long-Poll (RTC-only, kein esp_zb_lock)
static esp_err_t zigbee_status_handler(httpd_req_t *req) {
    last_web_activity_us = esp_timer_get_time();
    if (http_reject_unless_transfer_mode(req, TRANSFER_MODE_ZIGBEE, "ZigBee") != ESP_OK) {
        return ESP_OK;
    }

    char wait_param[8] = "";
    uint32_t wait_sec = 0;
    if (get_query_param(req, "wait", wait_param, sizeof(wait_param))) {
        wait_sec = (uint32_t)strtoul(wait_param, nullptr, 10);
        if (wait_sec > ZIGBEE_STATUS_WAIT_MAX_SEC) {
            wait_sec = ZIGBEE_STATUS_WAIT_MAX_SEC;
        }
    }

    uint32_t waited_ms = 0;
    bool timed_out = false;
    const bool long_poll = (wait_sec > 0 && !transfer_zigbee_rtc_config_valid());

    if (long_poll) {
        ESP_LOGI(TAG, "GET /zigbee/status Long-Poll (max %lu s, RTC-only)", (unsigned long)wait_sec);
        const uint32_t deadline_ms = wait_sec * 1000U;
        uint32_t since_activity_ms = 0;
        while (waited_ms < deadline_ms) {
            last_web_activity_us = esp_timer_get_time();
            if (transfer_zigbee_rtc_config_valid()) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(ZIGBEE_STATUS_WAIT_POLL_MS));
            waited_ms += ZIGBEE_STATUS_WAIT_POLL_MS;
            since_activity_ms += ZIGBEE_STATUS_WAIT_POLL_MS;
            if (since_activity_ms >= ZIGBEE_STATUS_ACTIVITY_REFRESH_MS) {
                since_activity_ms = 0;
                last_web_activity_us = esp_timer_get_time();
            }
        }
        timed_out = !transfer_zigbee_rtc_config_valid();
        if (timed_out) {
            waited_ms = deadline_ms;
        }
        ESP_LOGI(TAG, "/zigbee/status Long-Poll Ende: waited=%lu ms joined=%s",
                 (unsigned long)waited_ms, transfer_zigbee_rtc_config_valid() ? "ja" : "nein");
    } else {
        ESP_LOGI(TAG, "GET /zigbee/status (RTC-only, kein Stack-Lock)");
    }

    char json_response[576];
    const bool include_wait_meta = (wait_sec > 0);
    if (!zigbee_status_format_json(json_response, sizeof(json_response), waited_ms, include_wait_meta, timed_out)) {
        ESP_LOGW(TAG, "/zigbee/status: get_status_json fehlgeschlagen");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Fehler beim Ermitteln des ZigBee-Status\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static volatile bool zigbee_web_transfer_running = false;

/** Wie Timer-Wake transfer_data(): Zähler/Akku aus Runtime, ensure_joined + Reports. */
static void refresh_battery_measurements_for_transfer(void) {
    battery_adc_mv = read_adc_median_mv();
    battery_voltage = (float)battery_adc_mv / 1000.0f * VOLTAGE_DIVIDER_RATIO * config_rtc.adc_voltage_multiplier;
    battery_percent = VOLTAGE_TO_PERCENT(battery_voltage);
    ESP_LOGI(TAG, "Web-ZigBee-Transfer: ADC %lu mV → %.2f V, %.0f%%",
             (unsigned long)battery_adc_mv, battery_voltage, (float)battery_percent);
}

static void zigbee_web_transfer_task(void *pv) {
    (void)pv;
    if (zigbee_web_transfer_running) {
        ESP_LOGW(TAG, "zigbee_web_transfer_task: Läuft bereits");
        vTaskDelete(NULL);
        return;
    }
    zigbee_web_transfer_running = true;
    transfer_zigbee_set_web_transfer_busy(true);
    transfer_zigbee_mark_web_pairing_requested();

    refresh_battery_measurements_for_transfer();

    transfer_data_t data_to_transfer = {
        .pulse_counter = *(volatile uint32_t *)&ulp_pulse_counter,
        .battery_percent = (float)battery_percent,
        .battery_voltage = battery_voltage,
        .firmware_version = PROJECT_VERSION
    };

    ESP_LOGI(TAG, "Web-ZigBee-Transfer: pulse=%lu, Akku %.1f%% / %.2f V (wie Timer-Wake)",
             (unsigned long)data_to_transfer.pulse_counter,
             data_to_transfer.battery_percent, data_to_transfer.battery_voltage);

    transfer_status_t status = transfer_data(&data_to_transfer);

    if (status == TRANSFER_STATUS_OK) {
        ESP_LOGI(TAG, "Web-ZigBee-Transfer: erfolgreich (Join + Daten an Coordinator)");
    } else {
        ESP_LOGW(TAG, "Web-Zigbee-Transfer: %s (Status: %d)",
                 transfer_status_to_string(status), (int)status);
    }

    transfer_zigbee_clear_web_pairing_requested();
    transfer_zigbee_set_web_transfer_busy(false);
    zigbee_web_transfer_running = false;
    vTaskDelete(NULL);
}

// Handler für /zigbee/action (POST)
static esp_err_t zigbee_action_handler(httpd_req_t *req) {
    if (!http_require_config_auth(req)) {
        return http_send_401_unauthorized(req);
    }
    last_web_activity_us = esp_timer_get_time();
    if (http_reject_unless_transfer_mode(req, TRANSFER_MODE_ZIGBEE, "ZigBee") != ESP_OK) {
        return ESP_OK;
    }
    if (!wifi_is_connected()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req,
                        "{\"status\":\"error\",\"message\":\"ZigBee-Aktionen nur im WLAN (STA), nicht im Einrichtungs-AP.\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    char post_data[HTTP_ACTION_POST_MAX];
    int len = http_read_post_form(req, post_data, sizeof(post_data));
    if (len < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: POST-Daten ungültig", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // POST-Parameter prüfen: cmd
    char cmd[32] = "";
    if (!get_post_param(post_data, len, "cmd", cmd, sizeof(cmd))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: Parameter 'cmd' erforderlich", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "ZigBee-Action-Handler: cmd='%s'", cmd);
    
    if (strcmp(cmd, "factory-reset") == 0) {
        // Factory-Reset über Wrapper-Funktion
        bool success = transfer_zigbee_factory_reset(config_rtc.transfer_mode);
        
        if (success) {
            if (strcmp(config_rtc.transfer_mode, TRANSFER_MODE_ZIGBEE) == 0) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"status\":\"success\",\"message\":\"Factory-Reset erfolgreich. Bitte Gerät manuell neu starten, damit der ZigBee-Stack sauber initialisiert wird.\"}", HTTPD_RESP_USE_STRLEN);
            } else {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"status\":\"success\",\"message\":\"Factory-Reset erfolgreich (ZigBee war nicht aktiv).\"}", HTTPD_RESP_USE_STRLEN);
            }
        } else {
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Factory-Reset durchgeführt, aber Fehler beim Neuinitialisieren des ZigBee-Stacks. Bitte Gerät neu starten.\"}", HTTPD_RESP_USE_STRLEN);
        }
        
        return ESP_OK;
        
    } else if (strcmp(cmd, "start-pairing") == 0) {
        if (zigbee_web_transfer_running) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req,
                            "{\"status\":\"error\",\"message\":\"ZigBee-Übertragung/Pairing läuft bereits im Hintergrund.\"}",
                            HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        /* Gleicher Pfad wie Timer-Wake: prepare_cluster_attrs, ensure_joined, Reports (blockiert nicht HTTP). */
        BaseType_t created = xTaskCreate(zigbee_web_transfer_task, "zigbee_xfer", 8192, NULL, 5, NULL);
        if (created != pdPASS) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req,
                            "{\"status\":\"error\",\"message\":\"ZigBee-Task konnte nicht gestartet werden.\"}",
                            HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req,
                        "{\"status\":\"success\",\"message\":\"Pairing/Übertragung gestartet (wie Timer-Wake). Status per Long-Poll (/zigbee/status?wait=20).\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
        
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Fehler: Unbekannter Befehl (erwartet: factory-reset oder start-pairing)", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
}

// Handler für /mqtt/action (POST)
static esp_err_t mqtt_action_handler(httpd_req_t* req) {
    if (!http_require_config_auth(req)) {
        return http_send_401_unauthorized(req);
    }
    last_web_activity_us = esp_timer_get_time();
    if (http_reject_unless_transfer_mode(req, TRANSFER_MODE_MQTT, "MQTT") != ESP_OK) {
        return ESP_OK;
    }

    char post_data[HTTP_ACTION_POST_MAX];
    int len = http_read_post_form(req, post_data, sizeof(post_data));
    if (len < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"POST-Daten ungueltig\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char cmd[32] = "";
    if (!get_post_param(post_data, len, "cmd", cmd, sizeof(cmd)) || strcmp(cmd, "servertest") != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Parameter cmd=servertest erforderlich\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char host[MQTT_HOST_MAX_LEN + 1] = "";
    char port_str[8] = "";
    char username[MQTT_USERNAME_MAX_LEN + 1] = "";
    char password[MQTT_PASSWORD_MAX_LEN + 1] = "";

    if (!get_post_param(post_data, len, "host", host, sizeof(host))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Parameter host erforderlich\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (!get_post_param(post_data, len, "port", port_str, sizeof(port_str))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Parameter port erforderlich\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    get_post_param(post_data, len, "username", username, sizeof(username));
    get_post_param(post_data, len, "password", password, sizeof(password));

    int port_val = atoi(port_str);
    if (port_val < 1 || port_val > 65535) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"MQTT Port ungueltig (1-65535)\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char json_response[384];
    transfer_mqtt_test_connection(host, (uint16_t)port_val, username, password, json_response,
                                  sizeof(json_response));

    httpd_resp_set_type(req, "application/json");
    if (strstr(json_response, "\"status\":\"ok\"") != nullptr) {
        httpd_resp_send(req, json_response, HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, json_response, HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

// ============================================
// BLE-HTTP-Handler
// ============================================

static esp_err_t ble_status_handler(httpd_req_t *req) {
    last_web_activity_us = esp_timer_get_time();
    if (http_reject_unless_transfer_mode(req, TRANSFER_MODE_BLE, "BLE") != ESP_OK) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    char json_buf[128];
    transfer_ble_get_status_json(json_buf, sizeof(json_buf));
    httpd_resp_send(req, json_buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Einmal-Task: BLE-Pairing im Hintergrund, damit der HTTP-Handler sofort antwortet
   (transfer_ble_init() blockiert sonst mehrere Sekunden → Browser NetworkError). */
static void ble_pairing_task(void *pv) {
    (void)pv;
    transfer_ble_start_pairing();
    vTaskDelete(NULL);
}

static esp_err_t ble_action_handler(httpd_req_t *req) {
    if (!http_require_config_auth(req)) {
        return http_send_401_unauthorized(req);
    }
    last_web_activity_us = esp_timer_get_time();
    if (http_reject_unless_transfer_mode(req, TRANSFER_MODE_BLE, "BLE") != ESP_OK) {
        return ESP_OK;
    }

    char post_data[HTTP_ACTION_POST_MAX];
    int len = http_read_post_form(req, post_data, sizeof(post_data));
    if (len < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"POST-Daten ungueltig\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char cmd[32] = "";
    if (!get_post_param(post_data, len, "cmd", cmd, sizeof(cmd)) ||
        strcmp(cmd, "start-pairing") != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Parameter cmd=start-pairing erforderlich\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");

    BaseType_t created = xTaskCreate(ble_pairing_task, "ble_pair", 4096, NULL, 5, NULL);
    if (created != pdPASS) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"BLE-Task konnte nicht gestartet werden\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    /* Sofort 202 zurückgeben; BLE-Init/Advertising läuft im Hintergrund */
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"BLE Advertising gestartet (90 s)\"}", HTTPD_RESP_USE_STRLEN);
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

    wifi_mode_t mode_before = WIFI_MODE_STA;
    (void)esp_wifi_get_mode(&mode_before);
    const bool scan_in_ap_mode = (mode_before == WIFI_MODE_AP || mode_before == WIFI_MODE_APSTA);

    if (!wifi_manager_init()) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"WiFi-Scan fehlgeschlagen\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    if (scan_in_ap_mode) {
        if (!wifi_manager_prepare_scan_in_ap_mode()) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"WiFi-Modus APSTA fuer Scan fehlgeschlagen\"}",
                            HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    } else {
        ret = esp_wifi_set_mode(WIFI_MODE_STA);
        if (ret != ESP_OK) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"WiFi-Modus STA fehlgeschlagen\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        ret = esp_wifi_start();
        if (ret != ESP_OK) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"WiFi-Start fehlgeschlagen\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }

    wifi_manager_apply_tx_power();
    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_scan_config_t scan_config = {};
    wifi_manager_fill_scan_config(&scan_config, scan_in_ap_mode);
    
    ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        if (scan_in_ap_mode) {
            wifi_manager_restore_ap_after_scan();
        }
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"WiFi-Scan fehlgeschlagen\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    uint16_t ap_total = 0;
    esp_wifi_scan_get_ap_num(&ap_total);
    if (ap_total == 0) {
        if (scan_in_ap_mode) {
            wifi_manager_restore_ap_after_scan();
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    JsonDocument doc;
    JsonArray networksArray = doc.to<JsonArray>();
    wifi_ap_record_t rec;
    int n = 0;
    while (n < WIFI_SCAN_MAX_AP && esp_wifi_scan_get_ap_record(&rec) == ESP_OK) {
        JsonObject network = networksArray.add<JsonObject>();
        network["ssid"] = (const char*)rec.ssid;
        network["rssi"] = rec.rssi;
        network["encrypted"] = (rec.authmode != WIFI_AUTH_OPEN);
        n++;
    }
    esp_wifi_clear_ap_list();

    if (scan_in_ap_mode) {
        wifi_manager_restore_ap_after_scan();
    }

    char json_response[1024];
    serializeJson(doc, json_response, sizeof(json_response));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void ap_portal_index_url(char *buf, size_t len) {
    snprintf(buf, len, "http://%d.%d.%d.%d/index.html",
             AP_IP_ADDRESS_1, AP_IP_ADDRESS_2, AP_IP_ADDRESS_3, AP_IP_ADDRESS_4);
}

/** DHCP-Client erhaelt explizit AP-IP als DNS (nach Option-114-Restart). */
static void ap_dhcp_ensure_dns_server(void) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif == nullptr) {
        return;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return;
    }
    esp_netif_dhcps_stop(netif);
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip_info.ip.addr;
    esp_err_t dns_ret = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    if (dns_ret != ESP_OK) {
        ESP_LOGW(TAG, "AP DHCP set_dns_info: %s", esp_err_to_name(dns_ret));
    }
    uint8_t offer_dns = 1;
    esp_err_t opt_ret = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                                               ESP_NETIF_DOMAIN_NAME_SERVER,
                                               &offer_dns, sizeof(offer_dns));
    if (opt_ret != ESP_OK) {
        ESP_LOGW(TAG, "AP DHCP DNS-Option: %s", esp_err_to_name(opt_ret));
    }
    esp_err_t start_ret = esp_netif_dhcps_start(netif);
    if (start_ret != ESP_OK) {
        ESP_LOGW(TAG, "AP DHCP restart: %s", esp_err_to_name(start_ret));
    } else {
        ESP_LOGI(TAG, "AP DHCP DNS-Server: " IPSTR, IP2STR(&ip_info.ip));
    }
}

/** mDNS im AP-Modus: gas-o-meter2.local (STA-Pfad startet mDNS erst nach Join). */
static void ap_mdns_start(void) {
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mDNS AP init: %s", esp_err_to_name(ret));
        return;
    }
    mdns_hostname_set(config_rtc.hostname);
    mdns_instance_name_set("Gas-O-Meter");
    ret = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (ret == ESP_OK) {
        mdns_initialized = true;
        ESP_LOGI(TAG, "mDNS AP: http://%s.local", config_rtc.hostname);
    } else if (ret == ESP_ERR_INVALID_STATE) {
        mdns_initialized = true;
        ESP_LOGI(TAG, "mDNS AP: http://%s.local (bereits aktiv)", config_rtc.hostname);
    } else {
        ESP_LOGW(TAG, "mDNS AP service: %s", esp_err_to_name(ret));
    }
}

// Handler für / (Root)
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

/** Nur Pfad ohne ?query und #fragment (LittleFS-Dateinamen, Content-Type, Captive-Checks). */
static void uri_copy_path_only(char *dst, size_t dst_len, const char *uri) {
    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (!uri) {
        return;
    }
    size_t i = 0;
    for (; uri[i] && uri[i] != '?' && uri[i] != '#' && i + 1 < dst_len; ++i) {
        dst[i] = uri[i];
    }
    dst[i] = '\0';
}

static esp_err_t httpd_404_err_handler(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    return not_found_handler(req);
}

// Handler für /static/* (LittleFS data/static/)
static esp_err_t static_file_handler(httpd_req_t *req) {
    last_web_activity_us = esp_timer_get_time();

    char uri_path[128];
    uri_copy_path_only(uri_path, sizeof(uri_path), req->uri);

    char filepath[256];
    snprintf(filepath, sizeof(filepath), "/littlefs%s", uri_path);

    if (strstr(filepath, "config.json")) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    FILE* file = fopen(filepath, "r");
    if (!file) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "File not found", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // Content-Type bestimmen (.gz vor .css, sonst kein Content-Encoding bei .css.gz)
    if (strstr(uri_path, ".gz")) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        if (strstr(uri_path, ".css.gz")) {
            httpd_resp_set_type(req, "text/css");
        } else if (strstr(uri_path, ".js.gz")) {
            httpd_resp_set_type(req, "application/javascript");
        }
    } else if (strstr(uri_path, ".css")) {
        httpd_resp_set_type(req, "text/css");
    } else if (strstr(uri_path, ".js")) {
        httpd_resp_set_type(req, "application/javascript");
    } else if (strstr(uri_path, ".html")) {
        httpd_resp_set_type(req, "text/html");
    } else if (strstr(uri_path, ".png")) {
        httpd_resp_set_type(req, "image/png");
    } else if (strstr(uri_path, ".ico")) {
        httpd_resp_set_type(req, "image/x-icon");
    }
    
    // Cache-Header für bootstrap.min.css
    if (strstr(uri_path, "bootstrap.min.css")) {
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

void setupWebServer(bool enable_captive) {
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
    config.max_open_sockets = 13;  /* LWIP_MAX_SOCKETS(16) − 3 intern reserviert */
    config.max_uri_handlers = 40;  /* App(17) + /static/*(1) + Captive(11) */
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

    {
        esp_err_t eh = httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, httpd_404_err_handler);
        if (eh != ESP_OK) {
            ESP_LOGW(TAG, "HTTP-404-Error-Handler konnte nicht registriert werden: %s", esp_err_to_name(eh));
        }
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
    
    httpd_uri_t deepsleep_uri = {
        .uri       = "/deepsleep",
        .method    = HTTP_POST,
        .handler   = deepsleep_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &deepsleep_uri);
    
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
    
    httpd_uri_t pulse_counter_api_uri = {
        .uri       = "/api/pulse_counter",
        .method    = HTTP_GET,
        .handler   = pulse_counter_api_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &pulse_counter_api_uri);
    
    httpd_uri_t zigbee_status_uri = {
        .uri       = "/zigbee/status",
        .method    = HTTP_GET,
        .handler   = zigbee_status_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &zigbee_status_uri);
    
    httpd_uri_t zigbee_action_uri = {
        .uri       = "/zigbee/action",
        .method    = HTTP_POST,
        .handler   = zigbee_action_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &zigbee_action_uri);

    httpd_uri_t ble_status_uri = {
        .uri       = "/ble/status",
        .method    = HTTP_GET,
        .handler   = ble_status_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &ble_status_uri);

    httpd_uri_t ble_action_uri = {
        .uri       = "/ble/action",
        .method    = HTTP_POST,
        .handler   = ble_action_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &ble_action_uri);

    httpd_uri_t mqtt_action_uri = {
        .uri       = "/mqtt/action",
        .method    = HTTP_POST,
        .handler   = mqtt_action_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &mqtt_action_uri);

    httpd_uri_t static_uri = {
        .uri       = "/static/*",
        .method    = HTTP_GET,
        .handler   = static_file_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &static_uri);

    if (enable_captive) {
        ap_mdns_start();

        char portal_url[48];
        ap_portal_index_url(portal_url, sizeof(portal_url));
        captive_portal_config_t portal_cfg = CAPTIVE_PORTAL_CONFIG_DEFAULT();
        portal_cfg.redirect_url = portal_url;
        portal_cfg.netif_key = "WIFI_AP_DEF";

        esp_err_t cp_ret = captive_portal_register(server, &portal_cfg);
        if (cp_ret != ESP_OK) {
            ESP_LOGW(TAG, "Captive-Portal: %s", esp_err_to_name(cp_ret));
        } else {
            ap_dhcp_ensure_dns_server();
            ESP_LOGI(TAG, "Captive-Portal aktiv → %s (auch http://%s.local/)",
                     portal_url, config_rtc.hostname);
        }
        esp_log_level_set("captive_portal", ESP_LOG_WARN);
    }
    
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
    // Captive/Browser brechen viele HTTP-Verbindungen ab → harmlose WARN-Spam unterdrücken
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    // WiFi-Debug-Nachrichten reduzieren (muss ganz am Anfang stehen)
    SET_WIFI_LOG_LEVEL();
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Antennenumschaltung initialisieren (interne Antenne als Standard)
    INIT_ANTENNA_SWITCH(ANTENNA_INTERNAL);

    INIT_VBUS_DETECT_GPIO();
    ESP_LOGI(TAG, "Platinenversion: %d", BOARD_VERSION_ID);
    
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
    
    // WICHTIG: ADC-Messung bei JEDEM Wake-up durchführen (Power-On oder Timer/GPIO-Wake-up)
    // Diese Messung ist die Basis für alle weiteren Entscheidungen (Akku-Schutz, Übertragung, etc.)
    ESP_LOGI(TAG, "Führe ADC-Messung durch (bei jedem Wake-up)...");
    esp_err_t adc_ret = init_adc();
    if (adc_ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC Fehler: %s", esp_err_to_name(adc_ret));
        // Bei ADC-Fehler: Versuche trotzdem fortzufahren (kritisch, aber nicht fatal)
        battery_voltage = 0.0f;
        battery_percent = 0;
    } else {
        // Akku-Messung durchführen
        // HINWEIS: config_rtc.adc_voltage_multiplier ist IMMER gesetzt (entweder durch Initialisierung mit ADC_VOLTAGE_MULTIPLIER
        //          oder durch load_config() aus config.json). Daher können wir es direkt verwenden.
        battery_adc_mv = read_adc_median_mv();
        battery_voltage = (float)battery_adc_mv / 1000.0f * VOLTAGE_DIVIDER_RATIO * config_rtc.adc_voltage_multiplier;
        battery_percent = VOLTAGE_TO_PERCENT(battery_voltage);
        
        ESP_LOGI(TAG, "ADC-Messung: %d mV → Spannung: %.2f V, Prozent: %d%%", 
                 battery_adc_mv, battery_voltage, battery_percent);
        
        // Akku-Schutz-Prüfung: Bei zu niedriger Spannung sofort Deep-Sleep (ohne Übertragung)
        // WICHTIG: Diese Prüfung erfolgt BEVOR Config geladen wird, um Akku zu schützen!
        bool is_usb_power = IS_USB_POWER(battery_voltage);
        
        if (!is_usb_power && battery_voltage < BATTERY_VOLTAGE_20) {
            // Akku-Betrieb und Spannung < 20%: Sofort Deep-Sleep zum Akku-Schutz
            ESP_LOGW(TAG, "Akku zu niedrig - Sofort Deep-Sleep zum Akku-Schutz (ohne Übertragung)");
            ESP_LOGW(TAG, "Spannung: %.2f V (Minimum: %.2f V)", battery_voltage, BATTERY_VOLTAGE_20);
            
            // Vor Deep-Sleep: ulp_pulse_counter in Ring-Speicher schreiben (bei Akku-Low kann RTC-RAM verloren gehen)
            // WICHTIG: NVS muss vorher initialisiert werden!
            init_nvs_partitions(isPowerOn);
            init_ring_buffer_and_ulp_pulse_counter(isPowerOn);
            ESP_LOGI(TAG, "Speichere ulp_pulse_counter in Ring-Speicher vor Deep-Sleep (Akku-Low)...");
            write_ulp_pulse_counter_to_ring_buffer();
            
            // Deep-Sleep mit GPIO-Wake-up (Taster A) - Timer deaktiviert bei kritischer Spannung
            bool enable_timer = (battery_voltage > BATTERY_VOLTAGE_PROTECTION);
            enter_deep_sleep_with_gpio_and_timer_wakeup(enable_timer);
            // Ab hier wird Code nicht mehr ausgeführt (Deep-Sleep)
            return;
        }
    }
    
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
    
    // Stromversorgungs-Prüfung (basierend auf bereits durchgeführter ADC-Messung)
    // WICHTIG: Bei USB-Stromversorgung kann Timer aktiv bleiben (keine Akku-Probleme)
    bool is_usb_power = IS_USB_POWER(battery_voltage);
    bool enable_timer_wakeup = true;  // Standard: Timer aktiviert
    
    LOG_POWER_SUPPLY_MODE(TAG, battery_voltage, is_usb_power);
    if (is_usb_power) {
        // USB-Stromversorgung: Timer kann aktiv bleiben (keine Akku-Probleme)
        ESP_LOGI(TAG, "Betrieb fortgesetzt");
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

        // Zählerstand aus NVS/RTC laden, BEVOR der LP-Core (asynchron) gestartet wird.
        // Sonst kann ulp_lp_core_load_binary() den Wert auf 0 zurücksetzen.
        init_ring_buffer_and_ulp_pulse_counter(isPowerOn);

        // LP-Core Watchdog Task starten (asynchron)
        xTaskCreate(
            lp_core_watchdog_task,      // Task-Funktion
            "LP_Core_Watchdog",          // Task-Name
            4096,                        // Stack-Größe (Bytes)
            NULL,                        // Parameter
            1,                           // Priorität (niedrig, da nicht kritisch)
            NULL                         // Task-Handle (nicht benötigt)
        );
        ESP_LOGI(TAG, "LP-Core Watchdog Task gestartet");
        
        // 1. Batteriespannung-Test und ggf. in Ring-Speicher schreiben
        // < 30% ODER USB: Schreibe in Ring-Speicher (RTC-RAM könnte verloren gehen)
        // >= 30%: Kein Schreiben (RTC-RAM bleibt erhalten)
        if (battery_voltage < BATTERY_VOLTAGE_30 || IS_USB_POWER(battery_voltage)) {
            ESP_LOGI(TAG, "Speichere ulp_pulse_counter in Ring-Speicher (< 30%% oder USB)...");
            write_ulp_pulse_counter_to_ring_buffer();
        }
        
        // 2. Config laden (idempotent: prüft intern, ob bereits geladen)
        // WICHTIG: Muss vor allen Aktionen geladen sein, da beide Stränge (Timer/Power-On/GPIO) Config benötigen
        bool config_available = load_config();
        
        // 2.1 Battery-Spannung neu berechnen (mit korrektem adc_voltage_multiplier aus Config)
        // WICHTIG: Bei Power-On wurde battery_voltage VOR load_config() mit dem DEFAULT-Offset berechnet.
        //          Jetzt ist der korrekte Offset aus config.json geladen → Neuberechnung nötig!
        //          Bei Deep-Sleep-Wake-Up ist der RTC-Wert bereits korrekt, aber Neuberechnung schadet nicht.
        if (config_available && battery_adc_mv > 0) {
            battery_voltage = (float)battery_adc_mv / 1000.0f * VOLTAGE_DIVIDER_RATIO * config_rtc.adc_voltage_multiplier;
            battery_percent = VOLTAGE_TO_PERCENT(battery_voltage);
            ESP_LOGD(TAG, "Battery-Spannung neu berechnet (mit Config-Offset): %.2f V, %d%%", battery_voltage, battery_percent);
        }
        
        // 2a. ZigBee-Config initialisieren (lädt aus NVS bei Power-On, verwendet RTC-RAM bei Deep-Sleep-Wake-up)
        if (config_available && strcmp(config_rtc.transfer_mode, TRANSFER_MODE_ZIGBEE) == 0) {
            if (!zigbee_config_init(isPowerOn)) {
                ESP_LOGW(TAG, "ZigBee-Config-Initialisierung fehlgeschlagen");
            }
        }
        
        // 2b. Transfer-Modus initialisieren (einmalig beim Start)
        if (config_available) {
            if (!transfer_init(config_rtc.transfer_mode)) {
                ESP_LOGW(TAG, "Transfer-Initialisierung fehlgeschlagen (Mode: %s)", config_rtc.transfer_mode);
            }
        }
        
        // 3. Abhängig vom Wake-up-Grund: Unterschiedliche Aktionen
        const bool web_ui_wake = wake_allows_web_ui(wakeup_reason);
        switch (wakeup_reason) {
            case ESP_SLEEP_WAKEUP_TIMER: {
                // Timer-Wake-up: Übertragung alle N Timer-Wake-ups (transfer_minutes / wakeup_minutes)
                timer_wake_count++;
                if (timer_wake_should_transfer(config_available)) {
                    const uint8_t every_n = (uint8_t)(config_rtc.transfer_minutes / config_rtc.wakeup_minutes);
                    ESP_LOGI(TAG,
                             "=== Timer-Wake-up: Datenübertragung (Zähler %lu, alle %u Wake-ups, %u Min) ===",
                             (unsigned long)timer_wake_count, (unsigned)every_n,
                             (unsigned)config_rtc.transfer_minutes);
                    
                    // Transfer-Daten vorbereiten (ADC-Messung wurde bereits beim Wake-up durchgeführt)
                    // HINWEIS: battery_voltage und battery_percent wurden bereits beim Wake-up gemessen
                    //          und sind daher aktuell - keine erneute Messung nötig!
                    transfer_data_t data_to_transfer = {
                        .pulse_counter = *(volatile uint32_t *)&ulp_pulse_counter,
                        .battery_percent = (float)battery_percent,  // Bereits beim Wake-up gemessen
                        .battery_voltage = battery_voltage,  // Bereits beim Wake-up gemessen
                        .firmware_version = PROJECT_VERSION
                    };
                    
                    // Datenübertragung durchführen
                    transfer_status_t transfer_status = transfer_data(&data_to_transfer);
                    
                    if (transfer_status == TRANSFER_STATUS_OK) {
                        ESP_LOGI(TAG, "Datenübertragung erfolgreich abgeschlossen");
                    } else {
                        ESP_LOGW(TAG, "Datenübertragung fehlgeschlagen: %s (Status: %d)", 
                                transfer_status_to_string(transfer_status), transfer_status);
                    }
                    
                } else {
                    ESP_LOGI(TAG,
                             "=== Timer-Wake-up: Keine Übertragung (Zähler %lu, Intervall %u Min) ===",
                             (unsigned long)timer_wake_count, (unsigned)config_rtc.transfer_minutes);
                }
                ESP_LOGI(TAG, "Timer-Wake-up: Deep-Sleep (kein Web-Frontend)");
                enter_deep_sleep_after_wakeup(enable_timer_wakeup);
                break;
            }

            case ESP_SLEEP_WAKEUP_GPIO:
            case ESP_SLEEP_WAKEUP_EXT0:
            case ESP_SLEEP_WAKEUP_EXT1:
            case ESP_SLEEP_WAKEUP_UNDEFINED:
                // Taster oder Power-On: WiFi und Web-Server
                if (config_available) {
            ESP_LOGI(TAG, "Config erfolgreich geladen");
            
            // WiFi verbinden
            if (wifi_connect_sta()) {
                        // mDNS initialisieren (nur einmal, nach erfolgreicher WiFi-Verbindung mit IP)
                        // ESP-IDF Best Practice: mDNS nach IP-Adresszuweisung initialisieren
                        if (!mdns_initialized) {
                            esp_err_t mdns_ret = mdns_init();
                            if (mdns_ret == ESP_OK) {
                                // Hostname und Instance Name setzen (vor Service-Add)
                                mdns_hostname_set(config_rtc.hostname);
                                mdns_instance_name_set("Gas-O-Meter");
                                
                                // HTTP-Service hinzufügen
                                mdns_ret = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
                                if (mdns_ret == ESP_OK) {
                                    mdns_initialized = true;
                                    ESP_LOGI(TAG, "mDNS initialisiert: http://%s.local", config_rtc.hostname);
                                } else {
                                    ESP_LOGE(TAG, "mDNS: Fehler beim Hinzufügen des HTTP-Services: %s", esp_err_to_name(mdns_ret));
                                }
                            } else {
                                // ESP_ERR_INVALID_STATE bedeutet, dass mDNS bereits initialisiert wurde
                                if (mdns_ret == ESP_ERR_INVALID_STATE) {
                                    mdns_initialized = true;
                                    ESP_LOGW(TAG, "mDNS bereits initialisiert");
                                } else {
                                    ESP_LOGE(TAG, "mDNS-Initialisierung fehlgeschlagen: %s", esp_err_to_name(mdns_ret));
                                }
                            }
                        } else {
                            // mDNS bereits initialisiert - nur Hostname aktualisieren (falls geändert)
                            mdns_hostname_set(config_rtc.hostname);
                            ESP_LOGI(TAG, "mDNS: http://%s.local (bereits initialisiert)", config_rtc.hostname);
                        }
                        
                // NTP-Zeitsynchronisation
                sync_ntp_time();
                
                // Web-Server starten
                setupWebServer(false);
                        char ip_str[16];
                        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&wifi_get_ip_info()->ip));
                        ESP_LOGI(TAG, "Web-Server: http://%s", ip_str);
                    } else {
                        ESP_LOGE(TAG, "WiFi-Verbindung fehlgeschlagen → Starte Access Point");
                        wifi_manager_session_end();

                        /* AP zuerst: httpd nach laufendem AP starten (sonst TCP-Timeout trotz DNS). */
                        if (wifi_start_access_point()) {
                            setupWebServer(true);
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

            default:
                ESP_LOGW(TAG, "Wake-up 0x%x ohne Web-UI → Deep-Sleep", (unsigned)wakeup_reason);
                enter_deep_sleep_after_wakeup(enable_timer_wakeup);
                break;
        }

        if (web_ui_wake) {
            xTaskCreate(
                web_timeout_task,
                "web_timeout",
                4096,
                NULL,
                1,
                NULL);
        }
}

// ============================================
// FreeRTOS Task: Web-Server Timeout und Deep-Sleep Management
// ============================================
void web_timeout_task(void *parameter) {
    const TickType_t check_interval = pdMS_TO_TICKS(1000);  // 1 Sekunde
    const uint64_t sleep_threshold_us = WIFI_WAIT_FOR_SLEEP * 60 * 1000000ULL;  // Minuten in Mikrosekunden
    const uint64_t recent_activity_threshold_us = 30 * 1000000ULL;  // 30 Sekunden in Mikrosekunden
    
    ESP_LOGI(TAG, "Web-Timeout Task gestartet");
    
    while (1) {
        // Reboot-Prüfung
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
            bool is_usb_power = IS_USB_POWER(battery_voltage);
            bool enable_timer = is_usb_power || (battery_voltage > BATTERY_VOLTAGE_PROTECTION);
            enter_deep_sleep_after_wakeup(enable_timer);
            
            // Wenn wir hier ankommen, wurde Deep-Sleep nicht gestartet (z.B. keine Wake-up-Quelle)
            // Flag zurücksetzen, um Endlosschleife zu vermeiden
            should_enter_deep_sleep = false;
            ESP_LOGW(TAG, "Deep-Sleep konnte nicht gestartet werden - System bleibt aktiv");
        }
        vTaskDelay(check_interval);
    }
}

