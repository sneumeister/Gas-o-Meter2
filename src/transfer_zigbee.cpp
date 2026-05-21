#include "transfer_zigbee.h"
#include "zigbee_config.h"
#include "hardware.h"
#include "version.h"
#include "time_sync.h"
#include <string.h>  // Für strlen(), memcpy()
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_type.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_endpoint.h"
#include "esp_zigbee_attribute.h"
#include "bdb/esp_zigbee_bdb_commissioning.h"
#include "nwk/esp_zigbee_nwk.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_basic.h"
#include "zcl/esp_zigbee_zcl_power_config.h"
#include "zcl/esp_zigbee_zcl_metering.h"
#include "zcl/esp_zigbee_zcl_diagnostics.h"
#include "zcl/esp_zigbee_zcl_ota.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_core.h"
#include "zdo/esp_zigbee_zdo_common.h"
#include "zdo/esp_zigbee_zdo_command.h"
#include "esp_zigbee_secur.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include <ctime>
#include <cstdio>
#if CONFIG_ESP_ZB_TRACE_ENABLE
#include "esp_zigbee_trace.h"
#endif

static const char* TAG = "transfer_zigbee";

// ============================================
// Globale Variablen
// ============================================
static bool zigbee_initialized = false;
static TaskHandle_t zigbee_main_task_handle = NULL;
static bool factory_reset_in_progress = false;  // Flag: Factory-Reset läuft gerade
static volatile bool stack_ready_signal_received = false;  // Flag: SKIP_STARTUP Signal empfangen
static volatile bool stack_init_failed = false;  // Flag: Stack-Initialisierung fehlgeschlagen
static volatile bool pairing_successful = false;  // Flag: Pairing erfolgreich abgeschlossen
static volatile bool rejoin_successful = false;  // Flag: Rejoin erfolgreich abgeschlossen
static volatile bool zigbee_rejoin_in_progress = false;  // ensure_joined Rejoin-Schleife aktiv (fuer DEVICE_REBOOT-Guard)
static volatile bool zigbee_pairing_in_progress = false;  // ensure_joined Pairing-Schleife aktiv (fuer DEVICE_REBOOT-Guard)
static volatile bool device_rebooted_during_rejoin = false;  // Flag: DEVICE_REBOOT (ESP_OK) während manuellem Rejoin empfangen
static volatile bool first_pairing_after_join = false;  // Flag: Erstes Pairing nach Join (für Interview-Wartezeit)
static volatile bool steering_failed = false;  // Flag: Network Steering fehlgeschlagen
static volatile bool steering_successful = false;  // Flag: Network Steering erfolgreich (für Timing-Verzögerung)
static uint32_t last_device_annce_sent_ms = 0;  // Debounce fuer esp_zb_zdo_device_announcement_req()
static volatile bool zigbee_time_sync_response_received = false;
static volatile bool zigbee_stack_device_annce_received = false;
static volatile bool zigbee_nvs_save_pending = false;  // NVS-Schreiben aus Signal-Handler deferren

static void zigbee_send_device_annce_if_needed(const char* reason);
static void zigbee_maybe_send_device_annce_on_rejoin(const char* reason);
static bool zigbee_poll_joined_after_reboot(uint32_t timeout_ms, bool for_pairing,
                                            uint32_t *poll_elapsed_ms_out);
static bool zigbee_try_handle_reboot_in_steering_wait(bool for_pairing,
                                                     uint32_t *poll_elapsed_ms_out);
static void zigbee_update_rtc_from_stack_impl(void);
static void zigbee_update_rtc_from_stack(void);
static void zigbee_push_cluster_vals_to_stack_if_joined(void);
static void zigbee_wait_for_time_sync_response(void);
static void zigbee_wait_for_first_pairing_interview(void);
static bool zigbee_refresh_parent_link_diagnostics_impl(bool verbose_log);
static bool zigbee_refresh_parent_link_diagnostics(bool verbose_log);
typedef struct {
    bool     joined;
    uint16_t network_addr;
    uint16_t pan_id;
    uint8_t  channel;
} zigbee_network_info_t;
static bool zigbee_network_info_valid(const zigbee_network_info_t *info);
static zigbee_network_info_t zigbee_read_network_info_impl(void);
static zigbee_network_info_t zigbee_read_network_info_locked(void);
static void zigbee_enable_rx_before_steering(void);
static void zigbee_restore_sleepy_rx_on_when_idle(void);
static void zigbee_restore_primary_channel_mask(void);
static bool zigbee_try_direct_bdb_rejoin(uint32_t timeout_ms, uint32_t *elapsed_ms_out);
static bool zigbee_wait_stack_ready_after_failed_direct_rejoin(uint32_t max_wait_ms, uint32_t *elapsed_ms_out);

// RTC-RAM Variable für ZigBee-Config (Definition)
// WICHTIG: Deklaration ist in zigbee_config.h als extern
RTC_DATA_ATTR zigbee_rtc_t zigbee_rtc = {
    .joined = false,
    .network_addr = ZIGBEE_INVALID_NETWORK_ADDR,
    .coord_addr = ZIGBEE_DEFAULT_COORD_ADDR,
    .pan_id = ZIGBEE_DEFAULT_PAN_ID,
    .channel = ZIGBEE_DEFAULT_CHANNEL,
    .extended_addr = ZIGBEE_DEFAULT_EXTENDED_ADDR
};

// TX-Power Zugriff (definiert in main_idf.cpp)
extern int8_t transfer_zigbee_get_tx_power_dbm(void);

// Persistente Variable für Battery Percentage (wird vom Power Config Cluster verwendet)
// WICHTIG: Diese Variable muss persistent sein, da der Cluster einen Pointer darauf speichert
// Initialwert: 100% (200 in Zigbee-Format: 0-200 = 0-100%)
static uint8_t battery_percentage_remaining = 200;  // 100% in Zigbee-Format (0-200)

// Persistente Variable für Battery Voltage (wird vom Power Config Cluster verwendet)
// WICHTIG: Battery Voltage ist uint8 in 100mV Einheiten (z.B. 35 = 3.5V, 40 = 4.0V)
// Initialwert: 40 (4.0V = voller Akku)
static uint8_t battery_voltage_zigbee = 40;  // 4.0V in Zigbee-Format (100mV Einheiten)

// Persistente Variable für Battery Alarm State (wird vom Power Config Cluster verwendet)
// WICHTIG: Battery Alarm State ist map32 (32-bit Bitmap)
// Bit 0 = Low Voltage Alarm (wenn battery_voltage < BATTERY_VOLTAGE_30 = 3.57V)
// Initialwert: 0 (kein Alarm)
static uint32_t battery_alarm_state = 0;  // Bitmap: Bit 0 = Low Voltage Alarm

// Persistente Variable für CurrentSummationDelivered (wird vom Metering Cluster verwendet)
// WICHTIG: Diese Variable muss persistent sein, da der Cluster einen Pointer darauf speichert
// Initialwert: 0 (wird bei jeder Datenübertragung aktualisiert)
static esp_zb_uint48_t current_summation_delivered = {.low = 0, .high = 0};

// Persistente Variablen für Multiplier und Divisor (wird vom Metering Cluster verwendet)
// WICHTIG: Diese Variablen müssen persistent sein, da der Cluster einen Pointer darauf speichert
// Initialwerte: Multiplier=1, Divisor=100 (aus zigbee_config.h)
static uint16_t metering_multiplier = ZIGBEE_METERING_MULTIPLIER;  // 1
static uint16_t metering_divisor = ZIGBEE_METERING_DIVISOR;        // 100

// Diagnostics Cluster (0x0B05): Parent-Link LQI/RSSI für Zigbee2MQTT
static uint8_t diag_last_lqi = 0;
static int8_t diag_last_rssi = 0;

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
        .network_addr = ZIGBEE_INVALID_NETWORK_ADDR,
        .coord_addr = ZIGBEE_DEFAULT_COORD_ADDR,
        .pan_id = ZIGBEE_DEFAULT_PAN_ID,
        .channel = ZIGBEE_DEFAULT_CHANNEL,
        .extended_addr = ZIGBEE_DEFAULT_EXTENDED_ADDR
    };
    zigbee_rtc = default_config;
    
    // Versuche aus NVS zu laden
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    ESP_LOGI(TAG, "zigbee_config_load_from_nvs: Versuche NVS-Namespace '%s' zu öffnen...", ZIGBEE_NVS_NAMESPACE);
    err = nvs_open(ZIGBEE_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "zigbee_config_load_from_nvs: NVS-Namespace '%s' existiert nicht (noch nicht gepaart, err=%s) → RTC-RAM auf Default-Werte gesetzt", 
                 ZIGBEE_NVS_NAMESPACE, esp_err_to_name(err));
        // RTC-RAM ist bereits auf Default-Werte gesetzt
        return true;  // Kein Fehler, einfach noch nicht gepaart
    }
    ESP_LOGI(TAG, "zigbee_config_load_from_nvs: NVS-Namespace '%s' erfolgreich geöffnet", ZIGBEE_NVS_NAMESPACE);
    
    // Lade alle Werte aus NVS
    size_t required_size = sizeof(zigbee_rtc_t);
    ESP_LOGI(TAG, "zigbee_config_load_from_nvs: Versuche Config-Blob zu laden (Key: '%s', erwartete Size: %zu Bytes)...", 
             ZIGBEE_NVS_KEY_CONFIG, sizeof(zigbee_rtc_t));
    err = nvs_get_blob(nvs_handle, ZIGBEE_NVS_KEY_CONFIG, &zigbee_rtc, &required_size);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK && required_size == sizeof(zigbee_rtc_t)) {
        ESP_LOGI(TAG, "zigbee_config_load_from_nvs: Config-Blob erfolgreich geladen (Size: %zu Bytes)", required_size);
        // Validiere geladene Daten
        if (zigbee_rtc.joined && ZIGBEE_IS_NETWORK_ADDR_VALID(zigbee_rtc.network_addr)) {
            ESP_LOGI(TAG, "zigbee_config_load_from_nvs: Config aus NVS geladen (joined: %s, network_addr: 0x%04X, pan_id: 0x%04X, channel: %d, extended_addr=0x%016llX)",
                     zigbee_rtc.joined ? "true" : "false", zigbee_rtc.network_addr, zigbee_rtc.pan_id, zigbee_rtc.channel,
                     (unsigned long long)zigbee_rtc.extended_addr);
            return true;
        } else {
            // Geladene Daten sind ungültig → auf Default-Werte zurücksetzen
            ESP_LOGW(TAG, "zigbee_config_load_from_nvs: Config in NVS gefunden, aber ungültig (joined: %s, network_addr: 0x%04X) → RTC-RAM auf Default-Werte gesetzt",
                     zigbee_rtc.joined ? "true" : "false", zigbee_rtc.network_addr);
            zigbee_rtc = default_config;
            return true;
        }
    } else {
        if (err != ESP_OK) {
            ESP_LOGI(TAG, "zigbee_config_load_from_nvs: Config nicht in NVS gefunden (nvs_get_blob fehlgeschlagen: %s, Size: %zu, erwartet: %zu) → RTC-RAM auf Default-Werte gesetzt",
                     esp_err_to_name(err), required_size, sizeof(zigbee_rtc_t));
        } else {
            ESP_LOGI(TAG, "zigbee_config_load_from_nvs: Config nicht in NVS gefunden (Size-Mismatch: %zu != %zu) → RTC-RAM auf Default-Werte gesetzt",
                     required_size, sizeof(zigbee_rtc_t));
        }
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
        ESP_LOGW(TAG, "zigbee_config_save_to_nvs: NVS-Namespace '%s' existiert nicht, versuche zu erstellen...", ZIGBEE_NVS_NAMESPACE);
        err = nvs_open(ZIGBEE_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "zigbee_config_save_to_nvs: NVS-Namespace '%s' konnte nicht geöffnet werden: %s",
                     ZIGBEE_NVS_NAMESPACE, esp_err_to_name(err));
            return false;
        }
        ESP_LOGI(TAG, "zigbee_config_save_to_nvs: NVS-Namespace '%s' erfolgreich erstellt", ZIGBEE_NVS_NAMESPACE);
    }
    
    // Wear-Leveling: Prüfe ob sich Daten geändert haben – nur schreiben wenn nötig
    zigbee_rtc_t stored;
    size_t stored_size = sizeof(zigbee_rtc_t);
    err = nvs_get_blob(nvs_handle, ZIGBEE_NVS_KEY_CONFIG, &stored, &stored_size);
    if (err == ESP_OK && stored_size == sizeof(zigbee_rtc_t) &&
        memcmp(&stored, &zigbee_rtc, sizeof(zigbee_rtc_t)) == 0) {
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "zigbee_config_save_to_nvs: Config unverändert → kein NVS-Schreibvorgang (Wear-Leveling)");
        return true;
    }
    
    // Speichere gesamte Config-Struktur als Blob
    ESP_LOGI(TAG, "zigbee_config_save_to_nvs: Speichere Config: joined=%s, network_addr=0x%04X, pan_id=0x%04X, channel=%d, extended_addr=0x%016llX",
             zigbee_rtc.joined ? "true" : "false", zigbee_rtc.network_addr, zigbee_rtc.pan_id, zigbee_rtc.channel,
             (unsigned long long)zigbee_rtc.extended_addr);
    err = nvs_set_blob(nvs_handle, ZIGBEE_NVS_KEY_CONFIG, &zigbee_rtc, sizeof(zigbee_rtc_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "zigbee_config_save_to_nvs: Fehler beim Speichern (nvs_set_blob): %s (Size: %zu Bytes)", 
                 esp_err_to_name(err), sizeof(zigbee_rtc_t));
        nvs_close(nvs_handle);
        return false;
    }
    ESP_LOGI(TAG, "zigbee_config_save_to_nvs: nvs_set_blob erfolgreich (Size: %zu Bytes)", sizeof(zigbee_rtc_t));
    
    // Commit
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "zigbee_config_save_to_nvs: Fehler beim Committen (nvs_commit): %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    ESP_LOGI(TAG, "zigbee_config_save_to_nvs: nvs_commit erfolgreich");
    nvs_close(nvs_handle);

    nvs_handle_t verify_handle;
    err = nvs_open(ZIGBEE_NVS_NAMESPACE, NVS_READONLY, &verify_handle);
    if (err == ESP_OK) {
        zigbee_rtc_t verify_data;
        size_t verify_size = sizeof(zigbee_rtc_t);
        err = nvs_get_blob(verify_handle, ZIGBEE_NVS_KEY_CONFIG, &verify_data, &verify_size);
        if (err == ESP_OK && verify_size == sizeof(zigbee_rtc_t)) {
            // Verifikation: Prüfe alle wichtigen Felder inkl. extended_addr
            if (verify_data.joined == zigbee_rtc.joined && 
                verify_data.network_addr == zigbee_rtc.network_addr &&
                verify_data.pan_id == zigbee_rtc.pan_id &&
                verify_data.channel == zigbee_rtc.channel &&
                verify_data.extended_addr == zigbee_rtc.extended_addr) {
                ESP_LOGI(TAG, "zigbee_config_save_to_nvs: Verifikation erfolgreich ✓ (joined=%s, addr=0x%04X, pan_id=0x%04X, channel=%d, extended_addr=0x%016llX)",
                         verify_data.joined ? "true" : "false", verify_data.network_addr, verify_data.pan_id, verify_data.channel,
                         (unsigned long long)verify_data.extended_addr);
            } else {
                ESP_LOGW(TAG, "zigbee_config_save_to_nvs: Verifikation: Daten stimmen nicht überein!");
                ESP_LOGW(TAG, "  → Gespeichert: joined=%s, addr=0x%04X, pan_id=0x%04X, channel=%d, extended_addr=0x%016llX",
                         zigbee_rtc.joined ? "true" : "false", zigbee_rtc.network_addr, zigbee_rtc.pan_id, zigbee_rtc.channel,
                         (unsigned long long)zigbee_rtc.extended_addr);
                ESP_LOGW(TAG, "  → Gelesen: joined=%s, addr=0x%04X, pan_id=0x%04X, channel=%d, extended_addr=0x%016llX",
                         verify_data.joined ? "true" : "false", verify_data.network_addr, verify_data.pan_id, verify_data.channel,
                         (unsigned long long)verify_data.extended_addr);
            }
        } else {
            ESP_LOGE(TAG, "zigbee_config_save_to_nvs: Verifikation fehlgeschlagen: %s (Size: %zu, erwartet: %zu)", 
                     esp_err_to_name(err), verify_size, sizeof(zigbee_rtc_t));
        }
        nvs_close(verify_handle);
    } else {
        ESP_LOGW(TAG, "zigbee_config_save_to_nvs: Verifikation: NVS-Namespace konnte nicht geöffnet werden: %s", esp_err_to_name(err));
    }
    
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
// ZigBee Signal Handler
// ============================================

/**
 * @brief ZigBee Signal Handler für Stack-Events
 * 
 * @param signal_struct ZigBee Signal-Struktur
 */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    if (!signal_struct || !signal_struct->p_app_signal) {
        ESP_LOGE(TAG, "Invalid Zigbee signal struct!");
        return;
    }
    
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = (esp_zb_app_signal_type_t)(*p_sg_p);
    
    switch (sig_type) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "ZigBee Signal: SKIP_STARTUP (Stack initialisiert)");
            stack_ready_signal_received = true;
            // HINWEIS: Extended Address wird im DEVICE_FIRST_START Handler gelesen
            // (zu diesem Zeitpunkt ist sie möglicherweise noch nicht verfügbar)
            break;
            
        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START: {
            ESP_LOGI(TAG, "ZigBee Signal: DEVICE_FIRST_START (Status: %s)", esp_err_to_name(err_status));
            if (err_status == ESP_OK) {
                ESP_LOGI(TAG, "        → Device gestartet (Factory-New: %s)", 
                         esp_zb_bdb_is_factory_new() ? "ja" : "nein");
                
                // Extended Address (IEEE Address) jetzt lesen (nach DEVICE_FIRST_START)
                // WICHTIG: Erst nach DEVICE_FIRST_START ist die Extended Address garantiert verfügbar
                // (wird aus NVRAM/Flash geladen und ist für Pairing/Interview erforderlich)
                esp_zb_ieee_addr_t ieee_addr;
                esp_zb_get_long_address(ieee_addr);
                // 8 Bytes in uint64_t konvertieren (little-endian: ieee_addr[0] ist LSB, ieee_addr[7] ist MSB)
                zigbee_rtc.extended_addr = 0;
                for (int i = 0; i < 8; i++) {
                    zigbee_rtc.extended_addr |= ((uint64_t)ieee_addr[i]) << (i * 8);
                }
                ESP_LOGI(TAG, "        → Extended Address (IEEE) gelesen: 0x%016llX", (unsigned long long)zigbee_rtc.extended_addr);
                
                esp_zb_zcl_status_t attr_status;
                
                // HINWEIS: Basic Cluster Attribute (Manufacturer Name, Model ID) werden bereits beim
                // Erstellen des Clusters gesetzt (in create_gas_meter_endpoint()). Sie sind daher
                // bereits verfügbar, wenn der Coordinator beim Pairing die Attribute liest.
                // Die späte Setzung hier ist nicht mehr nötig, aber als Fallback behalten.
                
                // Multiplier und Divisor Attribute aktualisieren (nach DEVICE_FIRST_START, vor Pairing)
                // WICHTIG: Die Attribute wurden bereits beim Cluster-Erstellen mit persistenter Variable hinzugefügt
                //          Hier aktualisieren wir nur die Werte (falls sich etwas geändert hat)
                //          Die persistente Variable wird automatisch vom Cluster verwendet
                //          Der externe Converter (gas-o-meter2.js) liest diese Werte beim Pairing für Skalierung
                ESP_LOGI(TAG, "        → Aktualisiere Metering Cluster Attribute (Multiplier/Divisor)...");
                
                // Direkt die persistente Variable aktualisieren (der Cluster verwendet einen Pointer darauf)
                metering_multiplier = ZIGBEE_METERING_MULTIPLIER;
                metering_divisor = ZIGBEE_METERING_DIVISOR;
                ESP_LOGI(TAG, "        → Multiplier/Divisor aktualisiert: Multiplier=%u, Divisor=%u", 
                         metering_multiplier, metering_divisor);
                ESP_LOGI(TAG, "        → Externer Converter (gas-o-meter2.js) liest diese Werte beim Pairing für Skalierung");
                
                // Optional: Versuche auch esp_zb_zcl_set_attribute_val() (kann fehlschlagen, wenn Attribut read-only ist)
                // Aber die direkte Variable-Aktualisierung sollte ausreichen
                attr_status = esp_zb_zcl_set_attribute_val(ZIGBEE_ENDPOINT_ID, ZIGBEE_CLUSTER_METERING, 
                                                            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                                            ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID, 
                                                            &metering_multiplier, false);
                if (attr_status != ESP_ZB_ZCL_STATUS_SUCCESS) {
                    ESP_LOGD(TAG, "        → Hinweis: esp_zb_zcl_set_attribute_val() für Multiplier fehlgeschlagen (Status: %d), aber Variable wurde direkt aktualisiert", attr_status);
                }
                
                attr_status = esp_zb_zcl_set_attribute_val(ZIGBEE_ENDPOINT_ID, ZIGBEE_CLUSTER_METERING, 
                                                            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                                            ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID, 
                                                            &metering_divisor, false);
                if (attr_status != ESP_ZB_ZCL_STATUS_SUCCESS) {
                    ESP_LOGD(TAG, "        → Hinweis: esp_zb_zcl_set_attribute_val() für Divisor fehlgeschlagen (Status: %d), aber Variable wurde direkt aktualisiert", attr_status);
                }
            } else {
                ESP_LOGE(TAG, "        → Stack-Initialisierung fehlgeschlagen: %s", esp_err_to_name(err_status));
                stack_init_failed = true;  // Fehler-Flag setzen
            }
            break;
        }
            
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            ESP_LOGI(TAG, "ZigBee Signal: DEVICE_REBOOT (Status: %s)", esp_err_to_name(err_status));
            if (err_status == ESP_OK) {
                // Stack wurde erfolgreich reinitialisiert
                // Automatischer Rejoin wird intern vom Stack gestartet, wenn gespeicherte Netzwerk-Informationen vorhanden sind
                ESP_LOGI(TAG, "        → Stack reinitialisiert, automatischer Rejoin wird intern versucht...");
                ESP_LOGI(TAG, "        → Warte bis zu %d ms auf automatischen Rejoin (konfigurierbar via ZIGBEE_AUTO_REJOIN_WAIT_TIMEOUT_MS)", 
                         ZIGBEE_AUTO_REJOIN_WAIT_TIMEOUT_MS);
                // Nur waehrend aktiver Pairing-/Rejoin-Schleife in ensure_joined
                const bool in_rejoin = zigbee_rejoin_in_progress && !rejoin_successful;
                const bool in_pairing = zigbee_pairing_in_progress && !pairing_successful;
                if (in_rejoin || in_pairing) {
                    device_rebooted_during_rejoin = true;
                } else {
                    ESP_LOGD(TAG, "        → DEVICE_REBOOT ignoriert (kein Pairing/Rejoin-Zyklus oder bereits erfolgreich)");
                }
            } else {
                ESP_LOGE(TAG, "        → Stack-Reinitialisierung fehlgeschlagen: %s", esp_err_to_name(err_status));
                stack_init_failed = true;  // Fehler-Flag setzen
            }
            break;
            
        case ESP_ZB_BDB_SIGNAL_STEERING:
            if (err_status == ESP_OK) {
                ESP_LOGI(TAG, "ZigBee Signal: STEERING erfolgreich (Join laut SDK abgeschlossen)");
                ESP_LOGI(TAG, "        → PAN ID: 0x%04X, Channel: %d, Short Address: 0x%04X",
                         esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
                steering_successful = true;  // Flag: ensure_joined wartet ZIGBEE_STEERING_TO_ASSOCIATION_DELAY_MS
                ESP_LOGI(TAG, "        → STEERING OK - ensure_joined wartet bis %d ms vor Join-Pruefung",
                         ZIGBEE_STEERING_TO_ASSOCIATION_DELAY_MS);
                
                // esp_zb_bdb_dev_joined() kann kurz nach STEERING noch false sein
                if (esp_zb_bdb_dev_joined()) {
                    uint16_t network_addr = esp_zb_get_short_address();
                    ESP_LOGI(TAG, "        → Device bereits joined (0x%04X) → DEVICE_ANNCE", network_addr);
                    zigbee_send_device_annce_if_needed("steering_joined");
                } else {
                    ESP_LOGI(TAG, "        → esp_zb_bdb_dev_joined() noch false (Stack-Stabilisierung folgt)");
                }
            } else {
                ESP_LOGW(TAG, "ZigBee Signal: STEERING fehlgeschlagen (Status: %s)", esp_err_to_name(err_status));
                steering_failed = true;  // Flag setzen, damit Warte-Schleife sofort abbricht
                steering_successful = false;
            }
            break;
            
        case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
            ESP_LOGI(TAG, "ZigBee Signal: DEVICE_ANNCE");
            // Prüfe, ob Device erfolgreich joined ist
            if (esp_zb_bdb_dev_joined()) {
                zigbee_stack_device_annce_received = true;
                uint16_t network_addr = esp_zb_get_short_address();
                ESP_LOGI(TAG, "        → Device erfolgreich joined (Network Address: 0x%04X)", network_addr);
                
                ESP_LOGI(TAG, "        → Lese Parent-Informationen aus Neighbor Table...");
                zigbee_refresh_parent_link_diagnostics_impl(true);
                
                // Prüfe, ob es ein Pairing (factory-new) oder Rejoin war
                // HINWEIS: Wenn wir hier sind, ist das Device bereits joined, also sollte
                // esp_zb_bdb_is_factory_new() false sein. Aber zur Sicherheit prüfen wir
                // den vorherigen Status über zigbee_rtc.joined
                if (!zigbee_rtc.joined) {
                    // Device war vorher nicht joined -> Pairing
                    ESP_LOGI(TAG, "        → Pairing erfolgreich abgeschlossen");
                    pairing_successful = true;
                    first_pairing_after_join = true;  // Flag setzen für Interview-Wartezeit
                    
                    // rx_on_when_idle wird in ensure_joined vor Steering temporaer aktiviert
                    // und bleibt während der gesamten aktiven Phase aktiv (bis Deep-Sleep)
                    
                    // RTC-Status aktualisieren
                    zigbee_rtc.joined = true;
                    zigbee_rtc.network_addr = network_addr;
                    zigbee_rtc.pan_id = esp_zb_get_pan_id();
                    zigbee_rtc.channel = esp_zb_get_current_channel();
                    // Extended Address ist bereits bei Stack-Initialisierung gelesen worden (hardware-basiert)
                    // Nur zur Sicherheit nochmal prüfen/aktualisieren (falls sich etwas geändert hat)
                    esp_zb_ieee_addr_t ieee_addr;
                    esp_zb_get_long_address(ieee_addr);
                    uint64_t current_extended_addr = 0;
                    for (int i = 0; i < 8; i++) {
                        current_extended_addr |= ((uint64_t)ieee_addr[i]) << (i * 8);
                    }
                    if (zigbee_rtc.extended_addr != current_extended_addr) {
                        ESP_LOGW(TAG, "        → Extended Address geändert (alt: 0x%016llX, neu: 0x%016llX)",
                                 (unsigned long long)zigbee_rtc.extended_addr, (unsigned long long)current_extended_addr);
                        zigbee_rtc.extended_addr = current_extended_addr;
                    }
                    // NVS-Schreiben deferren (kein vTaskDelay/NVS im Signal-Handler – blockiert Zigbee-Stack)
                    zigbee_nvs_save_pending = true;
                    ESP_LOGI(TAG, "        → ZigBee-Config NVS-Speichern angefordert (Pairing, Main-Loop)");
                } else {
                    // Device war bereits joined -> Rejoin
                    ESP_LOGI(TAG, "        → Rejoin erfolgreich abgeschlossen");
                    rejoin_successful = true;
                    // RTC-Status aktualisieren (falls sich etwas geändert hat)
                    zigbee_rtc.network_addr = network_addr;
                    zigbee_rtc.pan_id = esp_zb_get_pan_id();
                    zigbee_rtc.channel = esp_zb_get_current_channel();
                    zigbee_nvs_save_pending = true;
                    ESP_LOGI(TAG, "        → ZigBee-Config NVS-Speichern angefordert (Rejoin, Main-Loop)");
                }
            } else {
                ESP_LOGW(TAG, "        → DEVICE_ANNCE empfangen, aber Device nicht joined");
            }
            break;
            
        case ESP_ZB_ZDO_SIGNAL_LEAVE:
            ESP_LOGI(TAG, "ZigBee Signal: LEAVE (Status: %s)", esp_err_to_name(err_status));
            break;
            
        case ESP_ZB_ZDO_SIGNAL_PRODUCTION_CONFIG_READY:
            if (err_status == ESP_OK) {
                ESP_LOGI(TAG, "ZigBee Signal: PRODUCTION_CONFIG_READY (Production Configuration geladen)");
            } else {
                ESP_LOGI(TAG, "ZigBee Signal: PRODUCTION_CONFIG_READY (Keine Production Configuration vorhanden - normal für End Devices)");
            }
            break;
            
        default:
            ESP_LOGI(TAG, "ZigBee Signal: Unknown (Type: 0x%X, Status: %s)", 
                     sig_type, esp_err_to_name(err_status));
            break;
    }
}

// Forward-Deklaration für read_attr_resp_callback (wird von zigbee_core_action_callback aufgerufen)
static void read_attr_resp_callback(esp_zb_zcl_cmd_read_attr_resp_message_t *message);

/**
 * @brief Zigbee Core Action Callback – leitet Read Attribute Response an read_attr_resp_callback weiter
 */
static esp_err_t zigbee_core_action_callback(esp_zb_core_action_callback_id_t callback_id, const void *message) {
    if (callback_id == ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID && message) {
        read_attr_resp_callback((esp_zb_zcl_cmd_read_attr_resp_message_t *)message);
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

/**
 * @brief Device-Callback für Read Attribute Response
 * 
 * Wird aufgerufen, wenn eine Read Attribute Response empfangen wird.
 * Verarbeitet die Zeit vom Coordinator (Time Cluster) und gibt sie auf Serial aus.
 * Später: lokale RTC-Synchronisation.
 * 
 * @param message Read Attribute Response Message
 */
static void read_attr_resp_callback(esp_zb_zcl_cmd_read_attr_resp_message_t *message) {
    if (!message || !message->variables) {
        return;
    }
    if (message->info.cluster != ZIGBEE_CLUSTER_TIME) {
        return;
    }
    
    // Durchlaufe alle Variablen in der Response
    esp_zb_zcl_read_attr_resp_variable_t *var = message->variables;
    while (var != NULL) {
        if (var->attribute.id == ZIGBEE_ATTR_TIME_TIME) {
            if (var->status == ESP_ZB_ZCL_STATUS_SUCCESS &&
                (var->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U32 ||
                 var->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_UTC_TIME)) {
                uint32_t utc_time = *(uint32_t *)var->attribute.data.value;
                const uint32_t ZIGBEE_EPOCH_OFFSET = 946684800;  // Sekunden 1970→2000
                time_t zigbee_unix = (time_t)(utc_time + ZIGBEE_EPOCH_OFFSET);
                struct tm timeinfo;
                if (gmtime_r(&zigbee_unix, &timeinfo)) {
                    // Aktuelle ESP32-Zeit (RTC) vor Korrektur
                    time_t esp32_unix = time(NULL);
                    int64_t delta_sec = (int64_t)zigbee_unix - (int64_t)esp32_unix;
                    const char *delta_sign = (delta_sec >= 0) ? "+" : "";
                    const char *delta_hint = (delta_sec > 0) ? " (ESP32 hinterher)"
                                     : (delta_sec < 0) ? " (ESP32 voreilend)" : " (synchron)";
                    ESP_LOGI(TAG, "========== AKTUELLE ZEIT (Coordinator) ==========");
                    printf("  Datum/Zeit (UTC): %04d-%02d-%02d %02d:%02d:%02d\n",
                           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
                    ESP_LOGI(TAG, "  Delta ESP32 ↔ Zigbee: %s%lld s%s",
                             delta_sign, (long long)delta_sec, delta_hint);
                    ESP_LOGI(TAG, "  Zigbee UTC: %lu s | Unix: %lu", (unsigned long)utc_time, (unsigned long)zigbee_unix);
                    // Harte Zeitkorrektur (settimeofday) – gemeinsame Sub-Routine für NTP/ZigBee/BLE
                    zigbee_time_sync_response_received = true;
                    time_sync_set_hard(zigbee_unix, "ZigBee");
                    ESP_LOGI(TAG, "================================================");
                } else {
                    ESP_LOGW(TAG, "  Coordinator-Zeit: Konvertierung fehlgeschlagen (UTC: %lu)", (unsigned long)utc_time);
                }
            } else if (var->status != ESP_ZB_ZCL_STATUS_SUCCESS) {
                ESP_LOGW(TAG, "  Time Attribut-Status: %d", var->status);
            } else {
                ESP_LOGW(TAG, "  Time Attribut falscher Typ (erwartet U32, erhalten: %d)", var->attribute.data.type);
            }
            break;
        }
        var = var->next;
    }
}

/** NVS-Speichern aus esp_zb_app_signal_handler (nach stack_main_loop_iteration, nicht im Handler). */
static void zigbee_process_pending_nvs_save(void) {
    if (!zigbee_nvs_save_pending) {
        return;
    }
    // Flag erst nach erfolgreichem Save loeschen – bei Fehler Retry naechste Iteration
    if (zigbee_config_save_to_nvs()) {
        zigbee_nvs_save_pending = false;
        ESP_LOGI(TAG, "        → ZigBee-Config in NVS gespeichert (deferred)");
    } else {
        ESP_LOGE(TAG, "        → FEHLER: ZigBee-Config NVS-Speichern fehlgeschlagen – Retry naechste Iteration");
    }
}

/**
 * @brief ZigBee Main Loop Task
 * 
 * @param pvParameters Task-Parameter (nicht verwendet)
 */
static void zigbee_main_task(void *pvParameters) {
    ESP_LOGI(TAG, "ZigBee Main Loop Task gestartet");
    
    // Warte auf Stack-Initialisierung, wenn noch nicht initialisiert
    if (!zigbee_initialized) {
        ESP_LOGI(TAG, "        → Warte auf Stack-Initialisierung (SKIP_STARTUP Signal)...");
        stack_ready_signal_received = false;  // Flag zurücksetzen
        stack_init_failed = false;  // Fehler-Flag zurücksetzen
        const uint32_t timeout_ms = ZIGBEE_INIT_TIMEOUT_MS;
        const uint32_t poll_interval_ms = ZIGBEE_INIT_POLL_INTERVAL_MS;
        uint32_t elapsed_ms = 0;
        
        while (!stack_ready_signal_received && !stack_init_failed && elapsed_ms < timeout_ms) {
            esp_zb_stack_main_loop_iteration();  // Events verarbeiten (wichtig für Signal-Handler!)
            zigbee_process_pending_nvs_save();
            vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
            elapsed_ms += poll_interval_ms;
        }
        
        if (stack_init_failed) {
            ESP_LOGE(TAG, "        → Stack-Initialisierung fehlgeschlagen (Fehler-Signal empfangen)");
            ESP_LOGE(TAG, "        → Task wird beendet");
            zigbee_main_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }
        
        if (!stack_ready_signal_received) {
            ESP_LOGE(TAG, "        → Timeout: Stack-Initialisierung nicht abgeschlossen (nach %d ms)", timeout_ms);
            ESP_LOGE(TAG, "        → Mögliche Ursachen: fehlende zb_storage Partition, Hardware-Problem, oder SDK-Konfigurationsfehler");
            ESP_LOGE(TAG, "        → Task wird beendet");
            zigbee_main_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }
        
        ESP_LOGI(TAG, "        → Stack-Initialisierung abgeschlossen (SKIP_STARTUP Signal erhalten nach %d ms)", elapsed_ms);
        zigbee_initialized = true;
        stack_ready_signal_received = false;  // Flag zurücksetzen
    }
    
    // Normaler Main Loop
    while (zigbee_initialized) {
        esp_zb_stack_main_loop_iteration();  // Nicht deprecated, esp_zb_stack_main_loop() ist die infinite loop Version
        zigbee_process_pending_nvs_save();
        vTaskDelay(pdMS_TO_TICKS(ZIGBEE_MAIN_TASK_DELAY_MS));
    }
    
    ESP_LOGI(TAG, "ZigBee Main Loop Task beendet");
    zigbee_main_task_handle = NULL;
    vTaskDelete(NULL);
}

// ============================================
// ZigBee-Stack Initialisierung
// ============================================

/**
 * @brief Erstellt Custom Endpoint mit Clusters
 * 
 * @return esp_zb_ep_list_t* Endpoint-Liste oder NULL bei Fehler
 */
static esp_zb_ep_list_t* create_gas_meter_endpoint(void) {
    ESP_LOGI(TAG, "Erstelle Custom Endpoint mit Clusters...");
    
    // Endpoint-Liste erstellen
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    if (ep_list == NULL) {
        ESP_LOGE(TAG, "Fehler beim Erstellen der Endpoint-Liste");
        return NULL;
    }
    
    // Endpoint-Konfiguration
    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = ZIGBEE_ENDPOINT_ID,
        .app_profile_id = ZIGBEE_PROFILE_ID,  // Home Automation Profile (aus zigbee_config.h)
        .app_device_id = ZIGBEE_DEVICE_ID,  // Custom Device (aus zigbee_config.h)
        .app_device_version = ZIGBEE_DEVICE_VERSION,
    };
    
    // Cluster-Liste erstellen
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    if (cluster_list == NULL) {
        ESP_LOGE(TAG, "Fehler beim Erstellen der Cluster-Liste");
        return NULL;
    }
    
    // Basic Cluster hinzufügen (für Firmware-Version, Manufacturer Name, Model ID)
    // Power Source per Config setzen (nicht add_attr – sonst "attribute 0x7 already existed")
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_BATTERY,
    };
    esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    if (basic_cluster == NULL) {
        ESP_LOGE(TAG, "Fehler beim Erstellen des Basic Clusters");
        return NULL;
    }
    
    // WICHTIG: Attribute beim Erstellen des Clusters hinzufügen (nicht erst später im DEVICE_FIRST_START Signal)
    // Diese Attribute werden vom Coordinator beim Pairing gelesen und sind für die Geräteerkennung essentiell
    // ZCL String-Format: Pascal-Format = Längenbyte (1 Byte) + String-Zeichen (ohne Null-Terminator)
    // Beispiel: "Custom" (6 Zeichen) → {0x06, 'C', 'u', 's', 't', 'o', 'm'}
    // WICHTIG: Das SDK erwartet ein Byte-Array, NICHT einen C-String!
    // static: Stack registriert Pointer – keine Stack-Locals (dangling nach Return)
    static uint8_t manufacturer_name[32] = {};
    static uint8_t model_id[32] = {};
    static uint8_t sw_build_id[16] = {};
    static uint8_t app_version = 0;

    // Manufacturer Name im ZCL Pascal-Format vorbereiten
    const char* manufacturer_name_str = ZIGBEE_MANUFACTURER_NAME;
    uint8_t manufacturer_name_len = strlen(manufacturer_name_str);
    if (manufacturer_name_len > 31) {
        manufacturer_name_len = 31;  // Max 31 Zeichen (32 Bytes total mit Längenbyte)
    }
    manufacturer_name[0] = manufacturer_name_len;
    memcpy(&manufacturer_name[1], manufacturer_name_str, manufacturer_name_len);
    
    // Manufacturer Name hinzufügen (Attribute ID: 0x0004)
    esp_zb_basic_cluster_add_attr(basic_cluster, 
                                   ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, 
                                   manufacturer_name);
    ESP_LOGI(TAG, "  → Manufacturer Name hinzugefügt: %s (Länge: %d)", manufacturer_name_str, manufacturer_name_len);
    
    // Model ID im ZCL Pascal-Format vorbereiten
    const char* model_id_str = ZIGBEE_MODEL_ID;
    uint8_t model_id_len = strlen(model_id_str);
    if (model_id_len > 31) {
        model_id_len = 31;  // Max 31 Zeichen (32 Bytes total mit Längenbyte)
    }
    model_id[0] = model_id_len;
    memcpy(&model_id[1], model_id_str, model_id_len);
    
    // Model ID hinzufügen (Attribute ID: 0x0005)
    esp_zb_basic_cluster_add_attr(basic_cluster, 
                                   ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, 
                                   model_id);
    ESP_LOGI(TAG, "  → Model ID hinzugefügt: %s (Länge: %d)", model_id_str, model_id_len);
    
    // Application Version (Firmware-Version) hinzufügen (Attribute ID: 0x0001)
    // WICHTIG: appVersion ist uint8_t (0-255), nicht ein String
    // Wir verwenden die Hauptversion aus PROJECT_VERSION (z.B. "0.6.1" → 0)
    app_version = 0;
    if (strlen(PROJECT_VERSION) > 0) {
        // Erste Ziffer der Version extrahieren (z.B. "0.6.1" → 0, "1.2.3" → 1)
        char first_char = PROJECT_VERSION[0];
        if (first_char >= '0' && first_char <= '9') {
            app_version = first_char - '0';
        }
    }
    esp_zb_basic_cluster_add_attr(basic_cluster, 
                                   ESP_ZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID, 
                                   &app_version);
    ESP_LOGI(TAG, "  → Application Version hinzugefügt: %u (aus PROJECT_VERSION: %s)", app_version, PROJECT_VERSION);
    
    // Software Build ID (Firmware-Version als String) hinzufügen (Attribute ID: 0x4000)
    // WICHTIG: swBuildId ist ein String im ZCL Pascal-Format (Längenbyte + String)
    // Zigbee2MQTT zeigt normalerweise swBuildId in der Geräte-Übersicht an
    const char* sw_build_id_str = PROJECT_VERSION;  // Verwende PROJECT_VERSION als Firmware-Version
    uint8_t sw_build_id_len = strlen(sw_build_id_str);
    if (sw_build_id_len > 15) {
        sw_build_id_len = 15;  // Max 15 Zeichen (16 Bytes total mit Längenbyte) für swBuildId
    }
    sw_build_id[0] = sw_build_id_len;
    memcpy(&sw_build_id[1], sw_build_id_str, sw_build_id_len);
    
    esp_zb_basic_cluster_add_attr(basic_cluster, 
                                   ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, 
                                   sw_build_id);
    ESP_LOGI(TAG, "  → Software Build ID hinzugefügt: %s (Länge: %d)", sw_build_id_str, sw_build_id_len);
    
    // Power Source (0x03 = Battery) wird über basic_cfg gesetzt (esp_zb_basic_cluster_create)
    
    esp_err_t err = esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fehler beim Hinzufügen des Basic Clusters: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "  → Basic Cluster hinzugefügt (Endpoint: %d)", ZIGBEE_ENDPOINT_ID);
    
    // Power Configuration Cluster manuell erstellen (Branch zigbee_reporting_fix)
    // WICHTIG: esp_zb_power_config_cluster_create() legt intern bereits Default-Attribute
    //          OHNE REPORTABLE-Flag an. Nachfolgendes esp_zb_cluster_add_attr() schlaegt
    //          dann mit ESP_ERR_INVALID_ARG fehl ("Attribut existiert bereits"), die
    //          Access-Flags lassen sich nicht mehr aendern -> Reporting funktioniert nicht.
    //          Loesung: leere Attribute-Liste anlegen und ALLE Battery-Attribute selbst
    //          mit READ_ONLY | REPORTING registrieren (analog zum Metering-Cluster unten).
    ESP_LOGI(TAG, "  → Erstelle Power Configuration Cluster manuell (REPORTABLE Flags fuer Battery-Attribute)...");
    esp_zb_attribute_list_t *power_config_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
    if (power_config_cluster == NULL) {
        ESP_LOGE(TAG, "Fehler beim Erstellen des Power Configuration Clusters");
        return NULL;
    }

    // Battery Percentage Remaining (Attribute ID: 0x0021, uint8, 0-200 fuer 0-100%)
    err = esp_zb_cluster_add_attr(
        power_config_cluster,
        ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
        ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &battery_percentage_remaining
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Battery Percentage Attribut konnte nicht hinzugefuegt werden: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "  → Battery Percentage Attribut hinzugefuegt (REPORTABLE, Initial: %u = %.1f%%)",
             battery_percentage_remaining, battery_percentage_remaining / 2.0f);

    // Battery Voltage (Attribute ID: 0x0020, uint8 in 100mV Einheiten)
    err = esp_zb_cluster_add_attr(
        power_config_cluster,
        ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
        ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &battery_voltage_zigbee
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Battery Voltage Attribut konnte nicht hinzugefuegt werden: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "  → Battery Voltage Attribut hinzugefuegt (REPORTABLE, Initial: %u = %.1fV)",
             battery_voltage_zigbee, battery_voltage_zigbee / 10.0f);

    // Battery Alarm State (Attribute ID: 0x003E, 32-bit Bitmap; Bit 0 = Low Voltage Alarm)
    err = esp_zb_cluster_add_attr(
        power_config_cluster,
        ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
        ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_ALARM_STATE_ID,
        ESP_ZB_ZCL_ATTR_TYPE_U32,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &battery_alarm_state
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Battery Alarm State Attribut konnte nicht hinzugefuegt werden: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "  → Battery Alarm State Attribut hinzugefuegt (REPORTABLE, Initial: 0x%08lX, Bit 0 = Low Voltage Alarm)",
             (unsigned long)battery_alarm_state);
    
    err = esp_zb_cluster_list_add_power_config_cluster(cluster_list, power_config_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fehler beim Hinzufügen des Power Configuration Clusters: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "  → Power Configuration Cluster hinzugefügt (Endpoint: %d)", ZIGBEE_ENDPOINT_ID);
    
    // Metering Cluster manuell erstellen (wie im Referenzcode: IgnacioHR/ZigbeeGasCounter)
    // WICHTIG: Wir erstellen den Cluster manuell, um volle Kontrolle über Access-Flags zu haben
    //          (insbesondere ESP_ZB_ZCL_ATTR_ACCESS_REPORTING für CurrentSummationDelivered)
    //          esp_zb_metering_cluster_create() erstellt das Attribut bereits, daher können wir
    //          die Access-Flags nicht nachträglich ändern
    ESP_LOGI(TAG, "  → Erstelle Metering Cluster manuell (für volle Kontrolle über Access-Flags)...");
    esp_zb_attribute_list_t *metering_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_METERING);
    if (metering_cluster == NULL) {
        ESP_LOGE(TAG, "Fehler beim Erstellen des Metering Clusters");
        return NULL;
    }
    
    // Persistente Variablen für Metering Cluster Attribute
    static uint8_t device_status = 0x00;  // Status (8-bit Bitmap, 0 = keine Fehler)
    static uint64_t device_extended_status = 0x00;  // Extended Status (64-bit Bitmap)
    static uint8_t unit_of_measure = ZIGBEE_METERING_UNIT_OF_MEASURE;  // m³ (aus zigbee_config.h)
    static uint8_t summation_formatting = ZIGBEE_METERING_SUMMATION_FORMATTING;
    static uint8_t metering_device_type = ZIGBEE_METERING_DEVICE_TYPE;  // Gas Meter (aus zigbee_config.h)
    static uint8_t demand_formatting = 0;  // Demand Formatting (nicht verwendet, da kein InstantaneousDemand)
    
    // CurrentSummationDelivered Attribut (0x0000) - WICHTIG: Als REPORTABLE markiert!
    // WICHTIG: ESP_ZB_ZCL_ATTR_ACCESS_REPORTING hinzufügen, damit Zigbee2MQTT Reporting konfigurieren kann
    //          Der externe Converter (gas-o-meter2.js) konfiguriert Reporting manuell beim Pairing
    // HINWEIS: Wenn das Attribut bereits existiert (ESP_ERR_INVALID_ARG), können wir die Access-Flags nicht ändern
    //          In diesem Fall müssen wir den Cluster komplett neu erstellen oder die Access-Flags beim ersten Erstellen setzen
    err = esp_zb_cluster_add_attr(
        metering_cluster,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
        ESP_ZB_ZCL_ATTR_TYPE_U48,  // 48-bit unsigned integer
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,  // Read-only + Reportable
        &current_summation_delivered  // Pointer auf persistente Variable
    );
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
        ESP_LOGE(TAG, "  → Fehler: CurrentSummationDelivered Attribut konnte nicht hinzugefügt werden: %s", esp_err_to_name(err));
        return NULL;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        // Attribut existiert bereits - Access-Flags können nicht nachträglich geändert werden
        ESP_LOGW(TAG, "  → WARNUNG: CurrentSummationDelivered Attribut existiert bereits (ESP_ERR_INVALID_ARG)");
        ESP_LOGW(TAG, "  → Access-Flags können nicht nachträglich geändert werden - Attribut ist möglicherweise NICHT als REPORTABLE markiert");
        ESP_LOGW(TAG, "  → LÖSUNG: Cluster muss ohne automatische Attribut-Erstellung erstellt werden");
    } else {
        ESP_LOGI(TAG, "  → CurrentSummationDelivered Attribut hinzugefügt (REPORTABLE, Initial: low=%lu, high=%u)", 
                 current_summation_delivered.low, current_summation_delivered.high);
    }
    
    // Status Attribut (0x0001) - Optional, aber nützlich für Fehleranzeige
    err = esp_zb_cluster_add_attr(
        metering_cluster,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_ATTR_METERING_STATUS_ID,
        ESP_ZB_ZCL_ATTR_TYPE_8BITMAP,  // 8-bit Bitmap
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,  // Read-only + Reportable
        &device_status
    );
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  → Warnung: Status Attribut konnte nicht hinzugefügt werden: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "  → Status Attribut hinzugefügt (Initial: 0x%02X)", device_status);
    }
    
    // Unit of Measure Attribut (0x0300)
    err = esp_zb_cluster_add_attr(
        metering_cluster,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_ATTR_METERING_UNIT_OF_MEASURE_ID,
        ESP_ZB_ZCL_ATTR_TYPE_U8,  // 8-bit unsigned integer
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,  // Read-only
        &unit_of_measure
    );
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  → Warnung: Unit of Measure Attribut konnte nicht hinzugefügt werden: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "  → Unit of Measure Attribut hinzugefügt (Initial: 0x%02X = m³)", unit_of_measure);
    }
    
    // Summation Formatting Attribut (0x0303)
    err = esp_zb_cluster_add_attr(
        metering_cluster,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_ATTR_METERING_SUMMATION_FORMATTING_ID,
        ESP_ZB_ZCL_ATTR_TYPE_U8,  // 8-bit unsigned integer
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,  // Read-only
        &summation_formatting
    );
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  → Warnung: Summation Formatting Attribut konnte nicht hinzugefügt werden: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "  → Summation Formatting Attribut hinzugefügt (Initial: 0x%02X)", summation_formatting);
    }
    
    // Metering Device Type Attribut (0x0306)
    err = esp_zb_cluster_add_attr(
        metering_cluster,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_ATTR_METERING_METERING_DEVICE_TYPE_ID,
        ESP_ZB_ZCL_ATTR_TYPE_U8,  // 8-bit unsigned integer
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,  // Read-only
        &metering_device_type
    );
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  → Warnung: Metering Device Type Attribut konnte nicht hinzugefügt werden: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "  → Metering Device Type Attribut hinzugefügt (Initial: 0x%02X = Gas Meter)", metering_device_type);
    }
    
    // Extended Status Attribut (0x0307) - Optional
    err = esp_zb_cluster_add_attr(
        metering_cluster,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_ATTR_METERING_EXTENDED_STATUS_ID,
        ESP_ZB_ZCL_ATTR_TYPE_64BITMAP,  // 64-bit Bitmap
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,  // Read-only + Reportable
        &device_extended_status
    );
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  → Warnung: Extended Status Attribut konnte nicht hinzugefügt werden: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "  → Extended Status Attribut hinzugefügt (Initial: 0x%016llX)", (unsigned long long)device_extended_status);
    }
    
    // Multiplier Attribut explizit mit Pointer auf persistente Variable hinzufügen
    // WICHTIG: Der externe Converter (gas-o-meter2.js) liest divisor/multiplier aus dem Cluster
    //          für automatische Skalierung (raw / divisor * multiplier)
    err = esp_zb_cluster_add_attr(
        metering_cluster,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID,  // 0x0301
        ESP_ZB_ZCL_ATTR_TYPE_U16,  // 16-bit unsigned integer
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,  // Read-only (wie in ZCL-Spezifikation)
        &metering_multiplier  // Pointer auf persistente Variable
    );
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "  → Warnung: Multiplier Attribut konnte nicht hinzugefügt werden: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "  → Multiplier Attribut hinzugefügt (Initial: %u)", metering_multiplier);
    }
    
    // Divisor Attribut explizit mit Pointer auf persistente Variable hinzufügen
    // WICHTIG: Der externe Converter (gas-o-meter2.js) liest divisor/multiplier aus dem Cluster
    //          für automatische Skalierung (raw / divisor * multiplier)
    err = esp_zb_cluster_add_attr(
        metering_cluster,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID,  // 0x0302
        ESP_ZB_ZCL_ATTR_TYPE_U16,  // 16-bit unsigned integer
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,  // Read-only (wie in ZCL-Spezifikation)
        &metering_divisor  // Pointer auf persistente Variable
    );
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "  → Warnung: Divisor Attribut konnte nicht hinzugefügt werden: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "  → Divisor Attribut hinzugefügt (Initial: %u)", metering_divisor);
    }
    // WICHTIG: Verwende esp_zb_cluster_list_add_metering_cluster() (Standard-Funktion)
    //          Die Attribute wurden bereits VORHER mit korrekten Access-Flags hinzugefügt
    //          esp_zb_cluster_list_add_metering_cluster() sollte die bereits vorhandenen Attribute nicht überschreiben
    err = esp_zb_cluster_list_add_metering_cluster(cluster_list, metering_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fehler beim Hinzufügen des Metering Clusters: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "  → Metering Cluster zur Cluster-Liste hinzugefügt (Attribute wurden bereits mit REPORTABLE-Flag hinzugefügt)");
    
    // HINWEIS: Multiplier und Divisor Attribute werden NICHT hier gesetzt!
    // Grund: Der ZigBee-Stack ist zu diesem Zeitpunkt noch nicht vollständig initialisiert.
    // Die Attribute werden im SKIP_STARTUP Signal-Handler gesetzt (nach Stack-Start, vor Pairing),
    // damit sie beim Pairing vom Coordinator gelesen werden können.
    //
    // WICHTIG: currentSummationDelivered ist als REPORTABLE markiert (ESP_ZB_ZCL_ATTR_ACCESS_REPORTING)
    //          Der externe Converter (gas-o-meter2.js) konfiguriert Reporting manuell beim Pairing.
    //          Der Converter wendet divisor/multiplier automatisch an für Skalierung.
    //          HA erkennt automatisch: device_class: gas, state_class: total_increasing (durch unit: 'm³')
    
    ESP_LOGI(TAG, "  → Metering Cluster hinzugefügt (Endpoint: %d, Unit: m³, Multiplier: %d, Divisor: %d)", 
             ZIGBEE_ENDPOINT_ID, ZIGBEE_METERING_MULTIPLIER, ZIGBEE_METERING_DIVISOR);
    ESP_LOGI(TAG, "  → HINWEIS: Multiplier/Divisor werden im SKIP_STARTUP Handler gesetzt (nach Stack-Start)");
    ESP_LOGI(TAG, "  → CurrentSummationDelivered ist als REPORTABLE markiert (Zigbee2MQTT kann Reporting konfigurieren)");
    ESP_LOGI(TAG, "  → Externer Converter (gas-o-meter2.js) liest divisor/multiplier für Skalierung");
    ESP_LOGI(TAG, "  → Device sendet Daten bei jeder Datenübertragung (transfer_zigbee_send_data())");

    // Diagnostics Cluster: Last LQI/RSSI (Z2M liest 0x011C/0x011D bei Interview)
    ESP_LOGI(TAG, "  → Erstelle Diagnostics Cluster (Last LQI/RSSI)...");
    esp_zb_attribute_list_t *diagnostics_cluster =
        esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_DIAGNOSTICS);
    if (diagnostics_cluster == NULL) {
        ESP_LOGE(TAG, "Fehler beim Erstellen des Diagnostics Clusters");
        return NULL;
    }
    err = esp_zb_cluster_add_attr(
        diagnostics_cluster,
        ESP_ZB_ZCL_CLUSTER_ID_DIAGNOSTICS,
        ESP_ZB_ZCL_ATTR_DIAGNOSTICS_LAST_LQI_ID,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &diag_last_lqi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Diagnostics Last LQI konnte nicht hinzugefuegt werden: %s", esp_err_to_name(err));
        return NULL;
    }
    err = esp_zb_cluster_add_attr(
        diagnostics_cluster,
        ESP_ZB_ZCL_CLUSTER_ID_DIAGNOSTICS,
        ESP_ZB_ZCL_ATTR_DIAGNOSTICS_LAST_RSSI_ID,
        ESP_ZB_ZCL_ATTR_TYPE_S8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &diag_last_rssi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Diagnostics Last RSSI konnte nicht hinzugefuegt werden: %s", esp_err_to_name(err));
        return NULL;
    }
    err = esp_zb_cluster_list_add_diagnostics_cluster(cluster_list, diagnostics_cluster,
                                                      ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fehler beim Hinzufuegen des Diagnostics Clusters: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "  → Diagnostics Cluster hinzugefuegt (Last LQI/RSSI fuer Z2M)");

    // OTA Upgrade Cluster (Client) – Platzhalter; Cluster-Liste beim Pairing fixieren
    ESP_LOGI(TAG, "  → Erstelle OTA Upgrade Cluster (Client, Platzhalter)...");
    esp_zb_ota_cluster_cfg_t ota_cfg = {
        .ota_upgrade_file_version = RING_BUFFER_VERSION,
        .ota_upgrade_manufacturer = ZIGBEE_OTA_MANUFACTURER_ID,
        .ota_upgrade_image_type = ZIGBEE_OTA_IMAGE_TYPE_ID,
        .ota_upgrade_downloaded_file_ver = ESP_ZB_ZCL_OTA_UPGRADE_DOWNLOADED_FILE_VERSION_DEF_VALUE,
    };
    esp_zb_attribute_list_t *ota_cluster = esp_zb_ota_cluster_create(&ota_cfg);
    if (ota_cluster == NULL) {
        ESP_LOGE(TAG, "Fehler beim Erstellen des OTA Clusters");
        return NULL;
    }
    err = esp_zb_cluster_list_add_ota_cluster(cluster_list, ota_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fehler beim Hinzufuegen des OTA Clusters: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "  → OTA Cluster hinzugefuegt (Client, FileVersion=0x%08lX)",
             (unsigned long)ota_cfg.ota_upgrade_file_version);

    // Time Cluster hinzufügen (Client-Rolle für Zeit-Synchronisation)
    // WICHTIG: Time Cluster wird als CLIENT hinzugefügt, damit wir die Zeit vom Coordinator lesen können
    // HINWEIS: Für Client-Rolle sind keine Attribute nötig, da wir nur lesen
    //          Der Time Cluster wird nicht explizit zur Liste hinzugefügt, da wir nur Read Attribute Requests senden
    //          und keine Server-Funktionalität benötigen. Der Read Attribute Request funktioniert auch ohne
    //          expliziten Cluster in der Liste, da wir die Cluster-ID direkt im Request angeben.
    ESP_LOGI(TAG, "  → Time Cluster wird für Zeit-Synchronisation verwendet (Read Attribute Request ohne explizite Cluster-Registrierung)");
    
    // Cluster-Liste zum Endpoint hinzufügen
    err = esp_zb_ep_list_add_ep(ep_list, cluster_list, endpoint_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fehler beim Hinzufügen des Endpoints zur Liste: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "Endpoint erstellt (Endpoint: %d, Profile: 0x%04X, Device: 0x%04X)",
             endpoint_config.endpoint, endpoint_config.app_profile_id, endpoint_config.app_device_id);
    
    return ep_list;
}

void transfer_zigbee_prepare_cluster_attrs(const transfer_data_t* data) {
    if (data == nullptr) {
        return;
    }
    current_summation_delivered.low = data->pulse_counter;
    current_summation_delivered.high = 0;

    uint8_t bp = (uint8_t)(data->battery_percent * 2.0f);
    if (bp > 200) {
        bp = 200;
    }
    battery_percentage_remaining = bp;

    if (data->battery_voltage > 0.0f) {
        uint8_t bv = (uint8_t)(data->battery_voltage * 10.0f + 0.5f);
        if (bv > 255) {
            bv = 255;
        }
        battery_voltage_zigbee = bv;
        battery_alarm_state = (data->battery_voltage < BATTERY_VOLTAGE_30)
            ? (battery_alarm_state | 0x00000001)
            : (battery_alarm_state & 0xFFFFFFFE);
    } else {
        battery_alarm_state &= 0xFFFFFFFE;
    }

    ESP_LOGI(TAG,
             "Cluster-RAM fuer Endpoint: summ=%lu (low=%lu), batt=%u (%.1f%%), battV=%u (%.2fV)",
             (unsigned long)data->pulse_counter,
             (unsigned long)current_summation_delivered.low,
             (unsigned)battery_percentage_remaining,
             data->battery_percent,
             (unsigned)battery_voltage_zigbee,
             data->battery_voltage);
}

bool transfer_zigbee_init(void) {
    if (zigbee_initialized) {
        ESP_LOGW(TAG, "transfer_zigbee_init: Bereits initialisiert");
        return true;
    }
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ZigBee-Stack Initialisierung");
    ESP_LOGI(TAG, "========================================");
    
    esp_err_t err;  // Fehler-Variable für alle ZigBee-Stack-Operationen
    
    // [0/5] ZigBee Trace-Logging aktivieren (VOR Stack-Initialisierung)
    #if CONFIG_ESP_ZB_TRACE_ENABLE
        // Erweiterte Trace-Logging für Debugging von Association-Problemen
        // MAC-Subsystem: Zeigt MAC-Layer Details (Association Requests, ACKs, etc.)
        // NWK-Subsystem: Zeigt Network-Layer Details (Routing, Discovery, etc.)
        // BDB-Subsystem: Zeigt Commissioning Details (Steering, Pairing, etc.)
        esp_zb_set_trace_level_mask(ESP_ZB_TRACE_LEVEL_DEBUG, 
                                    ESP_ZB_TRACE_SUBSYSTEM_NWK | 
                                    ESP_ZB_TRACE_SUBSYSTEM_BDB | 
                                    ESP_ZB_TRACE_SUBSYSTEM_MAC);
        ESP_LOGI(TAG, "  [0/5] ZigBee Trace-Logging aktiviert (NWK + BDB + MAC, Level: DEBUG)");
        ESP_LOGI(TAG, "        → MAC-Trace aktiviert für Association Request/Response Debugging");
    #endif
    
    // [1/5] ZigBee-Stack Konfiguration
    ESP_LOGI(TAG, "  [1/5] ZigBee-Stack wird konfiguriert...");
    
    // End Device Timeout: Verwende ESP_ZB_ED_AGING_TIMEOUT_64MIN falls verfügbar, sonst SDK Default
    // WICHTIG: keep_alive muss kleiner als ed_timeout sein!
    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,  // End Device
        .install_code_policy = ZIGBEE_INSTALL_CODE_POLICY_DEFAULT,
        .nwk_cfg = {
            .zed_cfg = {
                .keep_alive = ZIGBEE_KEEP_ALIVE_DEFAULT
            }
        }
    };
    
    // End Device Timeout setzen (falls Konstante verfügbar)
    #ifdef ESP_ZB_ED_AGING_TIMEOUT_64MIN
        zb_nwk_cfg.nwk_cfg.zed_cfg.ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN;  // 64 Minuten (empfohlen)
    #endif
    
    ESP_LOGI(TAG, "        → Device Type: End Device (ZED)");
    #ifdef ESP_ZB_ED_AGING_TIMEOUT_64MIN
        ESP_LOGI(TAG, "        → End Device Timeout: 64 Minuten (ESP_ZB_ED_AGING_TIMEOUT_64MIN)");
    #else
        ESP_LOGI(TAG, "        → End Device Timeout: SDK Default (nicht explizit gesetzt)");
    #endif
    ESP_LOGI(TAG, "        → Keep Alive: %d ms (Poll-Intervall)", ZIGBEE_KEEP_ALIVE_DEFAULT);
    
    // [2/5] Stack initialisieren
    ESP_LOGI(TAG, "  [2/5] Stack wird initialisiert...");
    esp_zb_init(&zb_nwk_cfg);  // Gibt void zurück (keine Fehlerbehandlung möglich)
    ESP_LOGI(TAG, "        → Stack initialisiert");
    
    // Sleepy End Device: Radio aus wenn idle (Coordinator pollt / Parent CSL)
    esp_zb_set_rx_on_when_idle(false);
    ESP_LOGI(TAG, "        → RX-on-when-idle deaktiviert (Sleepy ED – RX nur bei Join/Steering in ensure_joined)");
    
    // TX Power setzen (nach Stack-Initialisierung)
    const int8_t zigbee_tx_power_dbm = transfer_zigbee_get_tx_power_dbm();
    esp_zb_set_tx_power(zigbee_tx_power_dbm);
    int8_t current_tx_power = 0;
    esp_zb_get_tx_power(&current_tx_power);
    ESP_LOGI(TAG, "        → TX Power gesetzt: %d dBm (requested: %d dBm)", current_tx_power, zigbee_tx_power_dbm);
    
    // Stabilisierungszeit nach TX Power Setzung (RF-Operationen benötigen Zeit zur Stabilisierung)
    vTaskDelay(pdMS_TO_TICKS(ZIGBEE_TX_POWER_STABILIZE_MS));
    ESP_LOGI(TAG, "        → TX Power Stabilisierung: %d ms", ZIGBEE_TX_POWER_STABILIZE_MS);
    
    // Minimum LQI für Network Join setzen
    // WICHTIG: Bei LQI=0 kann Join fehlschlagen, wenn Minimum LQI > 0 ist
    // 0 = deaktiviert (erlaubt Join auch bei sehr schwachem Signal/LQI=0)
    // 32 = Standard (empfohlen für Produktion, verhindert instabile Verbindungen)
    esp_zb_secur_network_min_join_lqi_set(ZIGBEE_MIN_JOIN_LQI);
    uint8_t current_min_lqi = esp_zb_secur_network_min_join_lqi_get();
    ESP_LOGI(TAG, "        → Minimum LQI für Join gesetzt: %u (0 = deaktiviert)", current_min_lqi);
    
    // HINWEIS: Extended Address wird erst nach SKIP_STARTUP Signal gelesen
    // (zu diesem Zeitpunkt ist sie noch nicht verfügbar)
    
    // [3/5] Device Endpoint mit Clusters erstellen und registrieren
    ESP_LOGI(TAG, "  [3/5] Device Endpoint wird erstellt...");
    esp_zb_ep_list_t *ep_list = create_gas_meter_endpoint();
    if (ep_list == NULL) {
        ESP_LOGE(TAG, "        → Fehler beim Erstellen des Endpoints");
        return false;
    }
    
    // Device registrieren
    err = esp_zb_device_register(ep_list);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "        → Fehler bei esp_zb_device_register(): %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "        → Device Endpoint registriert");
    
    // [4/5] Signal Handler und Callbacks registrieren
    ESP_LOGI(TAG, "  [4/5] Signal Handler und Callbacks werden registriert...");
    ESP_LOGI(TAG, "        → Signal Handler registriert (automatisch)");
    // Read Attribute Response: für Time Cluster (Zeit vom Coordinator)
    esp_zb_core_action_handler_register(zigbee_core_action_callback);
    ESP_LOGI(TAG, "        → Read-Attribute-Response Handler registriert (Time Cluster → Serial)");
    
    // Primary Network Channel setzen
    ESP_LOGI(TAG, "        → Setze Primary Network Channel...");
    esp_err_t channel_err = esp_zb_set_primary_network_channel_set(ZIGBEE_PRIMARY_CHANNEL_MASK);
    if (channel_err != ESP_OK) {
        ESP_LOGW(TAG, "        → Fehler beim Setzen des Channel Masks: %s", esp_err_to_name(channel_err));
    } else {
        ESP_LOGI(TAG, "        → Primary Network Channel gesetzt (Mask: 0x%08lX)", (unsigned long)ZIGBEE_PRIMARY_CHANNEL_MASK);
    }
    
    // [5/5] Stack starten
    ESP_LOGI(TAG, "  [5/5] Stack wird gestartet...");
    err = esp_zb_start(false);  // false = nicht als Coordinator
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "        → Fehler bei esp_zb_start(): %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "        → Stack gestartet");
    
    // Extended Address (IEEE Address) versuchen zu lesen (nach esp_zb_start)
    // WICHTIG: Laut SDK-Dokumentation ist die Extended Address nach esp_zb_init() verfügbar,
    // aber sicherer ist es, sie nach esp_zb_start() zu lesen. Sie wird auch im DEVICE_FIRST_START
    // Handler gelesen (als Fallback/Verifikation).
    esp_zb_ieee_addr_t ieee_addr;
    esp_zb_get_long_address(ieee_addr);
    // Prüfe, ob Extended Address gültig ist (nicht 0x0000000000000000)
    uint64_t temp_extended_addr = 0;
    for (int i = 0; i < 8; i++) {
        temp_extended_addr |= ((uint64_t)ieee_addr[i]) << (i * 8);
    }
    if (temp_extended_addr != 0) {
        zigbee_rtc.extended_addr = temp_extended_addr;
        ESP_LOGI(TAG, "        → Extended Address (IEEE) gelesen: 0x%016llX", (unsigned long long)zigbee_rtc.extended_addr);
    } else {
        ESP_LOGW(TAG, "        → Extended Address noch nicht verfügbar (wird im DEVICE_FIRST_START Handler gelesen)");
    }
    
    // Node Descriptor konfigurieren (nach esp_zb_start, vor Pairing)
    // WICHTIG: Diese Werte werden vom Coordinator beim Interview abgefragt (Node Descriptor Request)
    // und sind essentiell für erfolgreiches Interview. Ohne diese Einstellungen schlägt das Interview fehl.
    ESP_LOGI(TAG, "        → Setze Node Descriptor (Power Source / Manufacturer Code)...");
    
    // Power Source setzen: false = Battery Powered (da das Device batteriebetrieben ist)
    // WICHTIG: Muss nach esp_zb_start() aufgerufen werden!
    esp_zb_set_node_descriptor_power_source(false);  // false = Battery Powered
    ESP_LOGI(TAG, "        → Node Descriptor Power Source gesetzt: Battery Powered");
    
    // Manufacturer Code setzen (optional, aber empfohlen für Interview)
    // WICHTIG: Muss nach esp_zb_start() aufgerufen werden!
    // Verwende einen Standard-Manufacturer-Code (0x0000 = nicht spezifiziert, oder eigener Code)
    uint16_t manufacturer_code = 0x0000;  // 0x0000 = nicht spezifiziert (kann später angepasst werden)
    esp_zb_set_node_descriptor_manufacturer_code(manufacturer_code);
    ESP_LOGI(TAG, "        → Node Descriptor Manufacturer Code gesetzt: 0x%04X", manufacturer_code);
    
    // Stabilisierungszeit nach Node Descriptor Setzung (RF-Operationen benötigen Zeit zur Stabilisierung)
    vTaskDelay(pdMS_TO_TICKS(ZIGBEE_NODE_DESC_STABILIZE_MS));
    ESP_LOGI(TAG, "        → Node Descriptor Stabilisierung: %d ms", ZIGBEE_NODE_DESC_STABILIZE_MS);
    
    // Stabilisierungszeit nach Stack-Start (wichtig für zuverlässiges Pairing)
    // WICHTIG: Stack muss vollständig initialisiert sein, bevor Pairing gestartet wird
    vTaskDelay(pdMS_TO_TICKS(ZIGBEE_STACK_START_STABILIZE_MS));
    ESP_LOGI(TAG, "        → Stack-Start Stabilisierung: %d ms (vor Pairing)", ZIGBEE_STACK_START_STABILIZE_MS);
    
    // ZigBee Main Loop Task starten (wartet selbst auf Stack-Initialisierung)
    ESP_LOGI(TAG, "        → Starte ZigBee Main Loop Task...");
    stack_ready_signal_received = false;  // Flag zurücksetzen
    xTaskCreate(zigbee_main_task, "zigbee_main", ZIGBEE_MAIN_TASK_STACK_SIZE, NULL, ZIGBEE_MAIN_TASK_PRIORITY, &zigbee_main_task_handle);
    if (zigbee_main_task_handle == NULL) {
        ESP_LOGE(TAG, "        → Fehler beim Erstellen des ZigBee Main Loop Tasks");
        return false;
    }
    ESP_LOGI(TAG, "        → ZigBee Main Loop Task gestartet (wartet auf SKIP_STARTUP Signal)");
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ZigBee-Stack Initialisierung gestartet (asynchron)");
    ESP_LOGI(TAG, "========================================");
    
    // WICHTIG: zigbee_initialized wird vom Task gesetzt, wenn SKIP_STARTUP Signal kommt
    // NICHT hier setzen, da Stack noch nicht vollständig initialisiert ist!
    
    return true;
}

// ============================================
// Rejoin-Helfer (DEVICE_REBOOT, DEVICE_ANNCE, RTC)
// ============================================

static bool zigbee_network_info_valid(const zigbee_network_info_t *info) {
    return info->joined
        && ZIGBEE_IS_NETWORK_ADDR_VALID(info->network_addr)
        && (info->pan_id != 0x0000);
}

/** Stack-Netzinfo lesen – nur unter esp_zb_lock (oder im Signal-Handler). */
static zigbee_network_info_t zigbee_read_network_info_impl(void) {
    zigbee_network_info_t info = {};
    info.joined = esp_zb_bdb_dev_joined();
    info.network_addr = esp_zb_get_short_address();
    info.pan_id = esp_zb_get_pan_id();
    info.channel = esp_zb_get_current_channel();
    return info;
}

static zigbee_network_info_t zigbee_read_network_info_locked(void) {
    esp_zb_lock_acquire(portMAX_DELAY);
    zigbee_network_info_t info = zigbee_read_network_info_impl();
    esp_zb_lock_release();
    return info;
}

static bool zigbee_is_joined_with_valid_network(void) {
    zigbee_network_info_t info = zigbee_read_network_info_locked();
    return zigbee_network_info_valid(&info);
}

static bool zigbee_network_addr_stable_for_annce(void) {
    zigbee_network_info_t info = zigbee_read_network_info_locked();
    if (!zigbee_network_info_valid(&info)) {
        return false;
    }
    if (zigbee_rtc.joined && ZIGBEE_IS_NETWORK_ADDR_VALID(zigbee_rtc.network_addr)) {
        if (info.network_addr == zigbee_rtc.network_addr) {
            return true;
        }
        for (int i = 0; i < ZIGBEE_ADDR_STABILIZE_RETRY_COUNT; i++) {
            vTaskDelay(pdMS_TO_TICKS(ZIGBEE_ADDR_STABILIZE_RETRY_MS));
            info = zigbee_read_network_info_locked();
            if (info.network_addr == zigbee_rtc.network_addr) {
                return true;
            }
        }
        ESP_LOGW(TAG, "        → DEVICE_ANNCE uebersprungen: Addr 0x%04X != RTC 0x%04X",
                 info.network_addr, zigbee_rtc.network_addr);
        return false;
    }
    return true;
}

/** Elapsed ms from then_ms to now_ms; safe across uint32 wrap (~49.7 d esp_timer). */
static uint32_t zigbee_elapsed_ms(uint32_t now_ms, uint32_t then_ms) {
    if (now_ms >= then_ms) {
        return now_ms - then_ms;
    }
    return (UINT32_MAX - then_ms) + now_ms + 1u;
}

static void zigbee_send_device_annce_if_needed(const char* reason) {
    if (!zigbee_network_addr_stable_for_annce()) {
        return;
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (last_device_annce_sent_ms != 0 &&
        zigbee_elapsed_ms(now_ms, last_device_annce_sent_ms) < ZIGBEE_DEVICE_ANNCE_MIN_INTERVAL_MS) {
        ESP_LOGI(TAG, "        → DEVICE_ANNCE uebersprungen (Debounce %lu ms, reason=%s)",
                 (unsigned long)ZIGBEE_DEVICE_ANNCE_MIN_INTERVAL_MS, reason);
        return;
    }
    esp_zb_zdo_device_announcement_req();
    last_device_annce_sent_ms = now_ms;
    zigbee_network_info_t info = zigbee_read_network_info_locked();
    ESP_LOGI(TAG, "        → DEVICE_ANNCE gesendet (Addr 0x%04X, reason=%s)",
             info.network_addr, reason);
}

/** Rejoin: Stack sendet ZDO Device Announce selbst – kein esp_zb_zdo_device_announcement_req (vermeidet Z2M-Doppel-Event). */
static void zigbee_maybe_send_device_annce_on_rejoin(const char* reason) {
    (void)reason;
    ESP_LOGI(TAG, "        → DEVICE_ANNCE uebersprungen (Rejoin, Stack-ZDO-Announce)");
}

/** Parent aus Neighbor Table: LQI/RSSI in Diagnostics-Cluster + coord_addr (ohne Lock). */
static bool zigbee_refresh_parent_link_diagnostics_impl(bool verbose_log) {
    if (!esp_zb_bdb_dev_joined()) {
        return false;
    }
    esp_zb_nwk_info_iterator_t iter = ESP_ZB_NWK_INFO_ITERATOR_INIT;
    esp_zb_nwk_neighbor_info_s nbr_info;
    while (esp_zb_nwk_get_next_neighbor(&iter, &nbr_info) == ESP_OK) {
        if (nbr_info.relationship != ESP_ZB_NWK_RELATIONSHIP_PARENT) {
            continue;
        }
        diag_last_lqi = nbr_info.lqi;
        diag_last_rssi = nbr_info.rssi;
        uint16_t parent_short_addr = nbr_info.short_addr;
        uint64_t parent_ieee_addr = 0;
        for (int i = 0; i < 8; i++) {
            parent_ieee_addr |= ((uint64_t)nbr_info.ieee_addr[i]) << (i * 8);
        }
        zigbee_rtc.coord_addr = parent_short_addr;
        if (!verbose_log) {
            ESP_LOGD(TAG, "        → Diagnostics Parent LQI=%u RSSI=%d",
                     (unsigned)diag_last_lqi, (int)diag_last_rssi);
            return true;
        }
        const char* parent_type = (parent_short_addr == 0x0000) ? "Coordinator" : "Router";
        ESP_LOGI(TAG, "        → Parent gefunden:");
        ESP_LOGI(TAG, "           → Typ: %s", parent_type);
        ESP_LOGI(TAG, "           → Network Address: 0x%04X", parent_short_addr);
        ESP_LOGI(TAG, "           → Extended Address: 0x%016llX", (unsigned long long)parent_ieee_addr);
        ESP_LOGI(TAG, "           → LQI=%u, RSSI=%d dBm", (unsigned)diag_last_lqi, (int)diag_last_rssi);
        if (parent_short_addr != 0x0000) {
            ESP_LOGI(TAG, "           → HINWEIS: Device ist ueber Router verbunden (nicht direkt mit Coordinator)");
        }
        return true;
    }
    if (verbose_log) {
        ESP_LOGW(TAG, "        → WARNUNG: Kein Parent in Neighbor Table gefunden (moeglicherweise noch nicht aktualisiert)");
    }
    return false;
}

/** Wie _impl, mit esp_zb_lock (Aufruf aus Application-Task, nicht Signal-Handler). */
static bool zigbee_refresh_parent_link_diagnostics(bool verbose_log) {
    esp_zb_lock_acquire(portMAX_DELAY);
    bool ok = zigbee_refresh_parent_link_diagnostics_impl(verbose_log);
    esp_zb_lock_release();
    return ok;
}

/** Battery/Metering: no-op nach Join – Stack liest Werte lazy aus gebundenen Variablen. */
static void zigbee_push_cluster_vals_to_stack_if_joined(void) {
    // no-op: Stack haelt Pointer auf current_summation_delivered, battery_percentage_remaining,
    // battery_voltage_zigbee, battery_alarm_state – Werte werden lazy beim Read/Report gelesen.
    // set_attribute_val hier wuerde REPORTABLE-Flag setzen und einen ungeplanten Report ausloesen (TZ-292).
    // Werte werden einmalig via transfer_zigbee_prepare_cluster_attrs() vor esp_zb_device_register gesetzt.
    // if (!esp_zb_bdb_dev_joined()) {
    //     return;
    // }
    // esp_zb_zcl_set_attribute_val(ZIGBEE_ENDPOINT_ID, ZIGBEE_CLUSTER_METERING, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
    //     ZIGBEE_ATTR_METERING_CURRENT_SUMMATION_DELIVERED, &current_summation_delivered, false);
    // esp_zb_zcl_set_attribute_val(ZIGBEE_ENDPOINT_ID, ZIGBEE_CLUSTER_BATTERY, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
    //     ZIGBEE_ATTR_BATTERY_PERCENT, &battery_percentage_remaining, false);
    // esp_zb_zcl_set_attribute_val(ZIGBEE_ENDPOINT_ID, ZIGBEE_CLUSTER_BATTERY, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
    //     ZIGBEE_ATTR_BATTERY_VOLTAGE, &battery_voltage_zigbee, false);
    // esp_zb_zcl_set_attribute_val(ZIGBEE_ENDPOINT_ID, ZIGBEE_CLUSTER_BATTERY, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
    //     ZIGBEE_ATTR_BATTERY_ALARM_STATE, &battery_alarm_state, false);
}

static void zigbee_wait_for_time_sync_response(void) {
    uint32_t waited_ms = 0;
    const uint32_t poll_ms = ZIGBEE_TIME_SYNC_POLL_INTERVAL_MS;
    while (waited_ms < ZIGBEE_TIME_SYNC_WAIT_MS && !zigbee_time_sync_response_received) {
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        waited_ms += poll_ms;
    }
}

/** Nach erstem Pairing: DEVICE_ANNCE abwarten (oder Timeout), dann Post-Announce-Fenster fuer Z2M. */
static void zigbee_wait_for_first_pairing_interview(void) {
    const uint32_t poll_ms = ZIGBEE_INTERVIEW_POLL_INTERVAL_MS;
    uint32_t waited_ms = 0;

    if (zigbee_stack_device_annce_received) {
        ESP_LOGI(TAG, "  [3.5/4] DEVICE_ANNCE bereits empfangen → Post-Announce-Fenster (%lu s)",
                 (unsigned long)(ZIGBEE_INTERVIEW_POST_ANNCE_MS / 1000));
    } else {
        ESP_LOGI(TAG, "  [3.5/4] Warte auf DEVICE_ANNCE (max %lu s)...",
                 (unsigned long)(ZIGBEE_INTERVIEW_ANNCE_WAIT_MAX_MS / 1000));
        while (waited_ms < ZIGBEE_INTERVIEW_ANNCE_WAIT_MAX_MS && !zigbee_stack_device_annce_received) {
            vTaskDelay(pdMS_TO_TICKS(poll_ms));
            waited_ms += poll_ms;
        }
        if (zigbee_stack_device_annce_received) {
            ESP_LOGI(TAG, "        → DEVICE_ANNCE nach %lu ms", (unsigned long)waited_ms);
        } else {
            ESP_LOGW(TAG, "        → DEVICE_ANNCE Timeout nach %lu ms → Post-Announce-Fenster trotzdem",
                     (unsigned long)waited_ms);
        }
    }

    ESP_LOGI(TAG, "        → Post-Announce: Z2M Interview/Configure (%lu s)",
             (unsigned long)(ZIGBEE_INTERVIEW_POST_ANNCE_MS / 1000));
    waited_ms = 0;
    while (waited_ms < ZIGBEE_INTERVIEW_POST_ANNCE_MS) {
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        waited_ms += poll_ms;
    }
    ESP_LOGI(TAG, "        → Interview-Wartezeit abgeschlossen");
    zigbee_stack_device_annce_received = false;
    // RX war fuer Pairing/Interview noetig (Z2M Downlink); ab send_data-Schritt 4 Sleepy ED
    zigbee_restore_sleepy_rx_on_when_idle();
}

static void zigbee_update_rtc_from_stack_impl(void) {
    zigbee_network_info_t info = zigbee_read_network_info_impl();
    zigbee_rtc.joined = true;
    zigbee_rtc.network_addr = info.network_addr;
    zigbee_rtc.pan_id = info.pan_id;
    zigbee_rtc.channel = info.channel;
    esp_zb_ieee_addr_t ieee_addr;
    esp_zb_get_long_address(ieee_addr);
    uint64_t current_extended_addr = 0;
    for (int i = 0; i < 8; i++) {
        current_extended_addr |= ((uint64_t)ieee_addr[i]) << (i * 8);
    }
    zigbee_rtc.extended_addr = current_extended_addr;
    zigbee_refresh_parent_link_diagnostics_impl(false);
}

static void zigbee_update_rtc_from_stack(void) {
    esp_zb_lock_acquire(portMAX_DELAY);
    zigbee_update_rtc_from_stack_impl();
    esp_zb_lock_release();
}

static bool zigbee_poll_joined_after_reboot(uint32_t timeout_ms, bool for_pairing,
                                            uint32_t *poll_elapsed_ms_out) {
    ESP_LOGI(TAG, "        → DEVICE_REBOOT: Poll Join (%s, max %lu ms)...",
             for_pairing ? "Pairing" : "Rejoin", (unsigned long)timeout_ms);
    uint32_t elapsed_ms = 0;
    const uint32_t poll_ms = ZIGBEE_STEERING_POLL_INTERVAL_MS;
    while (elapsed_ms < timeout_ms) {
        if (zigbee_is_joined_with_valid_network()) {
            zigbee_network_info_t info = zigbee_read_network_info_locked();
            ESP_LOGI(TAG, "        → Join nach DEVICE_REBOOT OK (nach %lu ms), Addr 0x%04X, PAN 0x%04X",
                     (unsigned long)elapsed_ms, info.network_addr, info.pan_id);
            bool was_rtc_joined = zigbee_rtc.joined;
            zigbee_update_rtc_from_stack();
            if (for_pairing) {
                pairing_successful = true;
                if (!was_rtc_joined) {
                    first_pairing_after_join = true;
                }
            } else {
                rejoin_successful = true;
            }
            zigbee_push_cluster_vals_to_stack_if_joined();
            if (zigbee_config_save_to_nvs()) {
                ESP_LOGI(TAG, "        → ZigBee-Config in NVS gespeichert (nach DEVICE_REBOOT Poll)");
            }
            if (for_pairing) {
                zigbee_send_device_annce_if_needed("device_reboot_poll");
            } else {
                zigbee_maybe_send_device_annce_on_rejoin("device_reboot_poll");
            }
            if (poll_elapsed_ms_out) {
                *poll_elapsed_ms_out = elapsed_ms;
            }
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        elapsed_ms += poll_ms;
        if (elapsed_ms % 5000 == 0 && elapsed_ms > 0) {
            ESP_LOGI(TAG, "        → Warte Join nach DEVICE_REBOOT... (%lu ms / %lu ms)",
                     (unsigned long)elapsed_ms, (unsigned long)timeout_ms);
        }
    }
    ESP_LOGW(TAG, "        → Automatischer Rejoin nach DEVICE_REBOOT nicht erfolgreich (nach %lu ms)",
             (unsigned long)elapsed_ms);
    if (poll_elapsed_ms_out) {
        *poll_elapsed_ms_out = elapsed_ms;
    }
    return false;
}

/** @return true = Join erkannt, Steering-Schleife beenden; false = Fehler oder kein Reboot */
/** Nur vor Network Steering (Beacon Scan) – nicht fuer BDB INITIALIZATION noetig. */
static void zigbee_enable_rx_before_steering(void) {
    esp_zb_set_rx_on_when_idle(true);
    ESP_LOGI(TAG, "        → RX-on-when-idle vor Network Steering aktiviert (Status: %s)",
             esp_zb_get_rx_on_when_idle() ? "true" : "false");
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void zigbee_restore_sleepy_rx_on_when_idle(void) {
    esp_zb_set_rx_on_when_idle(false);
    ESP_LOGI(TAG, "        → RX-on-when-idle deaktiviert (Sleepy ED, Status: %s)",
             esp_zb_get_rx_on_when_idle() ? "true" : "false");
}

/** Nach jedem erfolgreichen Rejoin (BDB direkt oder Steering-Fallback). */
static void zigbee_finalize_rejoin_success(void) {
    zigbee_restore_sleepy_rx_on_when_idle();
}

static void zigbee_restore_primary_channel_mask(void) {
    esp_err_t err = esp_zb_set_primary_network_channel_set(ZIGBEE_PRIMARY_CHANNEL_MASK);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "        → Primary Channel-Mask Restore fehlgeschlagen: %s", esp_err_to_name(err));
    }
}

/**
 * Schneller Rejoin: BDB INITIALIZATION auf gespeichertem Kanal (zb_storage), kein Network Steering.
 * Aequivalent zu esp_zb_zdo_rejoin_network(false) – diese API ist in esp-zigbee-lib 1.x nicht exportiert.
 */
static bool zigbee_try_direct_bdb_rejoin(uint32_t timeout_ms, uint32_t *elapsed_ms_out) {
    ESP_LOGI(TAG, "        → Direkter Rejoin (INITIALIZATION, Ch %u, PAN 0x%04X, kein Full-Scan)",
             (unsigned)zigbee_rtc.channel, zigbee_rtc.pan_id);

    const uint32_t channel_mask = (1UL << zigbee_rtc.channel);
    esp_err_t ch_err = esp_zb_set_primary_network_channel_set(channel_mask);
    if (ch_err != ESP_OK) {
        ESP_LOGW(TAG, "        → Channel-Mask 0x%08lX fehlgeschlagen: %s",
                 (unsigned long)channel_mask, esp_err_to_name(ch_err));
    }

    esp_err_t err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "        → BDB INITIALIZATION fehlgeschlagen: %s", esp_err_to_name(err));
        zigbee_restore_primary_channel_mask();
        if (elapsed_ms_out) {
            *elapsed_ms_out = 0;
        }
        return false;
    }

    uint32_t elapsed_ms = 0;
    const uint32_t poll_ms = ZIGBEE_AUTO_REJOIN_POLL_INTERVAL_MS;
    while (elapsed_ms < timeout_ms) {
        if (device_rebooted_during_rejoin) {
            const uint32_t remaining_ms = (elapsed_ms < timeout_ms)
                                          ? (timeout_ms - elapsed_ms)
                                          : 0;
            uint32_t poll_elapsed_ms = 0;
            if (remaining_ms > 0 &&
                zigbee_poll_joined_after_reboot(remaining_ms, false, &poll_elapsed_ms)) {
                device_rebooted_during_rejoin = false;
                zigbee_restore_primary_channel_mask();
                if (elapsed_ms_out) {
                    *elapsed_ms_out = elapsed_ms + poll_elapsed_ms;
                }
                return true;
            }
            elapsed_ms += poll_elapsed_ms;
            device_rebooted_during_rejoin = false;
        }
        if (rejoin_successful || zigbee_is_joined_with_valid_network()) {
            if (!rejoin_successful) {
                ESP_LOGI(TAG, "        → Direkter Rejoin OK (nach %lu ms)", (unsigned long)elapsed_ms);
                zigbee_update_rtc_from_stack();
                rejoin_successful = true;
                zigbee_push_cluster_vals_to_stack_if_joined();
                zigbee_maybe_send_device_annce_on_rejoin("direct_bdb_rejoin");
                if (zigbee_config_save_to_nvs()) {
                    ESP_LOGI(TAG, "        → ZigBee-Config in NVS gespeichert (direkter Rejoin)");
                }
            }
            zigbee_restore_primary_channel_mask();
            if (elapsed_ms_out) {
                *elapsed_ms_out = elapsed_ms;
            }
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        elapsed_ms += poll_ms;
    }
    ESP_LOGW(TAG, "        → Direkter Rejoin Timeout (%lu ms) → Network Steering",
             (unsigned long)timeout_ms);
    zigbee_restore_primary_channel_mask();
    if (elapsed_ms_out) {
        *elapsed_ms_out = elapsed_ms;
    }
    return false;
}

/**
 * Nach INITIALIZATION-Timeout ist der Stack oft noch im Commissioning.
 * Warte auf DEVICE_REBOOT und internen Rejoin, bevor Network Steering startet.
 * @return true wenn Join erkannt (Steering entbehrlich)
 */
static bool zigbee_wait_stack_ready_after_failed_direct_rejoin(uint32_t max_wait_ms, uint32_t *elapsed_ms_out) {
    ESP_LOGI(TAG, "        → INITIALIZATION ohne Join – warte DEVICE_REBOOT/Join (max %lu ms)",
             (unsigned long)max_wait_ms);
    uint32_t waited_ms = 0;
    const uint32_t poll_ms = ZIGBEE_AUTO_REJOIN_POLL_INTERVAL_MS;

    while (waited_ms < max_wait_ms) {
        if (rejoin_successful || zigbee_is_joined_with_valid_network()) {
            if (!rejoin_successful) {
                ESP_LOGI(TAG, "        → Join nach INITIALIZATION-Timeout (nach %lu ms)", (unsigned long)waited_ms);
                zigbee_update_rtc_from_stack();
                rejoin_successful = true;
                zigbee_push_cluster_vals_to_stack_if_joined();
                zigbee_maybe_send_device_annce_on_rejoin("direct_rejoin_post_fail");
                if (zigbee_config_save_to_nvs()) {
                    ESP_LOGI(TAG, "        → ZigBee-Config in NVS gespeichert (Join nach INIT-Timeout)");
                }
            }
            if (elapsed_ms_out) {
                *elapsed_ms_out = waited_ms;
            }
            return true;
        }
        if (device_rebooted_during_rejoin) {
            ESP_LOGI(TAG, "        → DEVICE_REBOOT nach INITIALIZATION-Timeout (nach %lu ms)",
                     (unsigned long)waited_ms);
            const uint32_t reboot_poll_ms = (max_wait_ms > waited_ms) ? (max_wait_ms - waited_ms) : poll_ms;
            uint32_t poll_elapsed_ms = 0;
            if (zigbee_poll_joined_after_reboot(reboot_poll_ms, false, &poll_elapsed_ms)) {
                device_rebooted_during_rejoin = false;
                if (elapsed_ms_out) {
                    *elapsed_ms_out = waited_ms + poll_elapsed_ms;
                }
                return true;
            }
            device_rebooted_during_rejoin = false;
            if (elapsed_ms_out) {
                *elapsed_ms_out = waited_ms + poll_elapsed_ms;
            }
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        waited_ms += poll_ms;
    }
    device_rebooted_during_rejoin = false;
    ESP_LOGW(TAG, "        → Kein DEVICE_REBOOT innerhalb %lu ms – starte Network Steering",
             (unsigned long)max_wait_ms);
    if (elapsed_ms_out) {
        *elapsed_ms_out = waited_ms;
    }
    return false;
}

static bool zigbee_try_handle_reboot_in_steering_wait(bool for_pairing,
                                                     uint32_t *poll_elapsed_ms_out) {
    if (poll_elapsed_ms_out) {
        *poll_elapsed_ms_out = 0;
    }
    if (!device_rebooted_during_rejoin) {
        return false;
    }
    ESP_LOGI(TAG, "        → DEVICE_REBOOT waehrend Steering erkannt → warte auf internen Rejoin...");
    uint32_t poll_elapsed_ms = 0;
    if (zigbee_poll_joined_after_reboot(ZIGBEE_AUTO_REJOIN_WAIT_TIMEOUT_MS, for_pairing,
                                        &poll_elapsed_ms)) {
        steering_successful = true;
        device_rebooted_during_rejoin = false;
        if (poll_elapsed_ms_out) {
            *poll_elapsed_ms_out = poll_elapsed_ms;
        }
        return true;
    }
    ESP_LOGW(TAG, "        → Automatischer Rejoin nach DEVICE_REBOOT fehlgeschlagen → Retry Steering");
    steering_failed = true;
    device_rebooted_during_rejoin = false;
    if (poll_elapsed_ms_out) {
        *poll_elapsed_ms_out = poll_elapsed_ms;
    }
    return false;
}

/**
 * @brief Stellt sicher, dass das Device mit dem ZigBee-Netzwerk verbunden ist
 * 
 * Prüft den aktuellen Status (factory-new, joined) und startet bei Bedarf
 * Pairing oder Rejoin. Wartet auf erfolgreichen Abschluss mit Timeouts.
 * 
 * @return transfer_status_t TRANSFER_STATUS_OK wenn verbunden, Fehlercode bei Fehler
 */
transfer_status_t transfer_zigbee_ensure_joined(void) {
    // zigbee_stack_device_annce_received nicht hier zuruecksetzen – wird fuer
    // zigbee_wait_for_first_pairing_interview() nach Pairing benoetigt (sonst 60s Timeout)
    zigbee_rejoin_in_progress = false;
    zigbee_pairing_in_progress = false;

    // Status-Prüfung: factory-new? joined?
    ESP_LOGI(TAG, "transfer_zigbee_ensure_joined: Prüfe ZigBee-Status...");
    bool is_factory_new = esp_zb_bdb_is_factory_new();
    zigbee_network_info_t net_info = zigbee_read_network_info_locked();
    bool is_joined = net_info.joined;
    
    ESP_LOGI(TAG, "        → Factory-New: %s", is_factory_new ? "ja" : "nein");
    ESP_LOGI(TAG, "        → Joined: %s", is_joined ? "ja" : "nein");
    ESP_LOGI(TAG, "        → zigbee_rtc.joined: %s", zigbee_rtc.joined ? "true" : "false");
    ESP_LOGI(TAG, "        → zigbee_rtc.network_addr: 0x%04X", zigbee_rtc.network_addr);
    
    // WICHTIG: Prüfe auch zigbee_rtc.joined (aus NVS/RTC-RAM)
    // Wenn zigbee_rtc.joined=true, aber esp_zb_bdb_dev_joined()=false, bedeutet das:
    // - Device war bereits gepaart (Config in NVS vorhanden)
    // - Stack wurde neu initialisiert (z.B. nach Deep-Sleep-Wake-up)
    // - Device muss Rejoin machen, aber wir wissen, dass es bereits gepaart war
    // In diesem Fall sollten wir NICHT versuchen, Pairing zu machen, sondern Rejoin
    if (zigbee_rtc.joined && ZIGBEE_IS_NETWORK_ADDR_VALID(zigbee_rtc.network_addr)) {
        ESP_LOGI(TAG, "        → Device war bereits gepaart (Config in NVS/RTC-RAM vorhanden)");
        ESP_LOGI(TAG, "        → Passive Auto-Rejoin (max %d ms), danach ZDO-Rejoin oder Steering",
                 ZIGBEE_AUTO_REJOIN_PASSIVE_WAIT_MS);
        const uint32_t passive_wait_ms = ZIGBEE_AUTO_REJOIN_PASSIVE_WAIT_MS;
        const uint32_t poll_interval_ms = ZIGBEE_AUTO_REJOIN_POLL_INTERVAL_MS;
        uint32_t elapsed_ms = 0;

        while (!is_joined && elapsed_ms < passive_wait_ms) {
            vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
            elapsed_ms += poll_interval_ms;
            net_info = zigbee_read_network_info_locked();
            is_joined = net_info.joined;
        }
        
        if (is_joined) {
            ESP_LOGI(TAG, "        → Passiver Auto-Rejoin erfolgreich (nach %d ms)", elapsed_ms);
            uint16_t prev_addr = zigbee_rtc.network_addr;
            uint16_t prev_pan = zigbee_rtc.pan_id;
            uint8_t prev_ch = zigbee_rtc.channel;
            zigbee_update_rtc_from_stack();
            if (zigbee_rtc.network_addr != prev_addr || zigbee_rtc.pan_id != prev_pan
                    || zigbee_rtc.channel != prev_ch) {
                ESP_LOGI(TAG, "        → Netzwerk geaendert (Addr 0x%04X -> 0x%04X, PAN 0x%04X, Ch %u)",
                         prev_addr, zigbee_rtc.network_addr, zigbee_rtc.pan_id,
                         (unsigned)zigbee_rtc.channel);
                if (zigbee_config_save_to_nvs()) {
                    ESP_LOGI(TAG, "        → ZigBee-Config in NVS gespeichert (passiver Rejoin)");
                }
            }
            // Rejoin: Interview-Flag nur verwerfen wenn kein erstes Pairing in diesem Zyklus
            // (DEVICE_ANNCE vor ensure_joined setzt pairing_successful + first_pairing_after_join)
            if (!pairing_successful) {
                first_pairing_after_join = false;
            }
            return TRANSFER_STATUS_OK;
        } else {
            ESP_LOGW(TAG, "        → Passiver Auto-Rejoin nicht erfolgreich (nach %d ms) → ZDO-Rejoin/Steering",
                     elapsed_ms);
            // Weiter mit manuellem Rejoin (siehe unten)
        }
    }
    
    // Wenn bereits joined, nichts zu tun
    if (is_joined) {
        ESP_LOGI(TAG, "        → Device bereits joined → Kein Pairing/Rejoin nötig");
        
        // WICHTIG: Synchronisiere zigbee_rtc mit Stack-Status, falls nicht synchron
        // Dies kann passieren, wenn DEVICE_ANNCE Signal nicht empfangen wurde (z.B. nach fehlgeschlagener configure)
        // ODER wenn das Device zwischen Wake-ups joined ist (z.B. während eines Wake-ups ohne Datenübertragung)
        bool was_not_joined = !zigbee_rtc.joined || !ZIGBEE_IS_NETWORK_ADDR_VALID(zigbee_rtc.network_addr);
        if (was_not_joined) {
            ESP_LOGW(TAG, "        → WARNUNG: zigbee_rtc nicht synchron mit Stack-Status → synchronisiere...");
            ESP_LOGW(TAG, "        → Device hat wahrscheinlich zwischen Wake-ups gejoint (z.B. während Wake-up ohne Datenübertragung)");
            
            // Prüfe, ob es ein erstes Pairing war (factory-new Device, das gerade gejoint ist)
            // is_factory_new wird false, sobald das Device erfolgreich gejoint ist
            // Wenn zigbee_rtc.joined = false war, war es definitiv ein erstes Pairing
            bool is_first_pairing = was_not_joined && !is_factory_new;
            if (is_first_pairing) {
                ESP_LOGI(TAG, "        → Erstes Pairing erkannt (Device war factory-new und hat gerade gejoint)");
            }
            
            zigbee_update_rtc_from_stack();
            
            // RTC war leer, Stack joined → Interview-Fenster in send_data
            ESP_LOGI(TAG, "        → Device hat gerade gejoint → first_pairing_after_join (Interview-Wartezeit in send_data)");
            first_pairing_after_join = true;
            
            // In NVS speichern
            if (zigbee_config_save_to_nvs()) {
                ESP_LOGI(TAG, "        → zigbee_rtc erfolgreich synchronisiert und in NVS gespeichert");
            } else {
                ESP_LOGE(TAG, "        → FEHLER: zigbee_rtc synchronisiert, aber konnte nicht in NVS gespeichert werden!");
            }
        } else if (!pairing_successful) {
            // RTC synchron, kein Pairing in diesem Zyklus – veraltetes Interview-Flag verwerfen
            first_pairing_after_join = false;
        }
        
        return TRANSFER_STATUS_OK;
    }
    
    // Flags zurücksetzen für neuen Zyklus
    pairing_successful = false;
    rejoin_successful = false;
    first_pairing_after_join = false;  // Zurücksetzen (nur beim ersten Pairing nach Join setzen)
    steering_failed = false;
    
    // Pairing/Rejoin-Logik
    const uint32_t cycle_timeout_ms = ZIGBEE_CYCLE_TIMEOUT_MS;
    const uint32_t pairing_timeout_ms = ZIGBEE_PAIRING_TIMEOUT_MS;
    const uint32_t rejoin_timeout_ms = ZIGBEE_JOIN_TIMEOUT_MS;  // 30 Sekunden für Rejoin
    const uint32_t poll_interval_ms = ZIGBEE_STEERING_POLL_INTERVAL_MS;  // Längeres Poll-Intervall für Network Steering (weniger CPU-Last)
    const uint32_t steering_retry_count = ZIGBEE_STEERING_RETRY_COUNT;
    const uint32_t steering_retry_timer_ms = ZIGBEE_STEERING_RETRY_TIMER_MS;
    uint32_t cycle_start_ms = 0;  // Wird gesetzt, wenn Pairing/Rejoin startet
    uint32_t elapsed_ms = 0;
    
    if (is_factory_new) {
        // Pairing-Logik mit Retry-Mechanismus
        ESP_LOGI(TAG, "        → Device ist factory-new → Starte Pairing...");
        zigbee_pairing_in_progress = true;
        // rx_on_when_idle wird in ensure_joined vor Steering temporaer aktiviert
        
        uint32_t steering_attempt = 0;
        bool pairing_completed = false;
        
        while (steering_attempt < steering_retry_count && !pairing_completed && elapsed_ms < cycle_timeout_ms) {
            // Flags zurücksetzen für neuen Versuch
            pairing_successful = false;
            steering_failed = false;
            steering_successful = false;
            
            if (steering_attempt > 0) {
                ESP_LOGI(TAG, "        → Retry-Versuch %d/%d (nach %d ms Wartezeit)...", 
                         steering_attempt, steering_retry_count, steering_retry_timer_ms);
                vTaskDelay(pdMS_TO_TICKS(steering_retry_timer_ms));
                elapsed_ms += steering_retry_timer_ms;
            }
            
            // WICHTIG: RX-on-when-idle explizit VOR Network Steering aktivieren
            // Dies stellt sicher, dass das Device den Network Key vom Trust Center empfangen kann
            // Auch wenn es bereits in der Initialisierung gesetzt wurde, setzen wir es hier nochmal,
            // um sicherzustellen, dass es wirklich aktiv ist (Timing-Sicherheit)
            esp_zb_set_rx_on_when_idle(true);
            bool rx_on_when_idle_status = esp_zb_get_rx_on_when_idle();
            ESP_LOGI(TAG, "        → RX-on-when-idle vor Network Steering aktiviert (Status: %s)", 
                     rx_on_when_idle_status ? "true" : "false");
            
            // WICHTIG: Kurze Verzögerung nach rx_on_when_idle, damit der Stack Zeit hat, es zu aktivieren
            // Dies reduziert "Have not got nwk key - authentication failed" Fehler beim ersten Pairing-Versuch
            const uint32_t rx_on_when_idle_stabilize_ms = 100;  // 100ms Verzögerung
            vTaskDelay(pdMS_TO_TICKS(rx_on_when_idle_stabilize_ms));
            ESP_LOGI(TAG, "        → RX-on-when-idle Stabilisierung: %d ms", rx_on_when_idle_stabilize_ms);
            
            esp_err_t comm_err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            if (comm_err != ESP_OK) {
                ESP_LOGE(TAG, "        → Fehler beim Starten von Network Steering (Versuch %d): %s", 
                         steering_attempt + 1, esp_err_to_name(comm_err));
                steering_attempt++;
                continue;  // Nächster Retry-Versuch
            }
            
            ESP_LOGI(TAG, "        → Network Steering gestartet (Versuch %d/%d, Timeout: %d ms)", 
                     steering_attempt + 1, steering_retry_count , pairing_timeout_ms);
            cycle_start_ms = elapsed_ms;
            
            // Warte auf Network Steering Erfolg (mit Timeout)
            // WICHTIG: 500ms Poll-Intervall gibt dem Stack genug Zeit, Signale zu verarbeiten
            // (200ms war zu kurz und könnte Race Conditions verursachen)
            const uint32_t steering_poll_interval_ms = 500;  // 500ms für zuverlässige Signal-Verarbeitung (konsistent mit ZIGBEE_STEERING_POLL_INTERVAL_MS)
            uint32_t steering_wait_start_ms = elapsed_ms;
            while (!steering_successful && !steering_failed && elapsed_ms < cycle_timeout_ms && 
                   (elapsed_ms - steering_wait_start_ms) < ZIGBEE_NETWORK_DISCOVERY_MS) {
                uint32_t reboot_poll_ms = 0;
                if (zigbee_try_handle_reboot_in_steering_wait(true, &reboot_poll_ms)) {
                    elapsed_ms += reboot_poll_ms;
                    break;
                }
                elapsed_ms += reboot_poll_ms;
                vTaskDelay(pdMS_TO_TICKS(steering_poll_interval_ms));
                elapsed_ms += steering_poll_interval_ms;
                if (steering_failed) {
                    ESP_LOGI(TAG, "        → Network Steering fehlgeschlagen erkannt (nach %d ms) → Breche sofort ab", 
                             elapsed_ms - steering_wait_start_ms);
                    break;
                }
            }
            
            if (pairing_successful) {
                pairing_completed = true;
                break;
            }
            
            if (steering_successful) {
                ESP_LOGI(TAG, "        → STEERING OK - warte %d ms (Stack-Stabilisierung vor Join-Pruefung)",
                         ZIGBEE_STEERING_TO_ASSOCIATION_DELAY_MS);
                vTaskDelay(pdMS_TO_TICKS(ZIGBEE_STEERING_TO_ASSOCIATION_DELAY_MS));
                elapsed_ms += ZIGBEE_STEERING_TO_ASSOCIATION_DELAY_MS;
            }
            
            // Warte auf Pairing-Erfolg (mit Timeout) oder Steering-Fehler
            // HINWEIS: pairing_successful wird im DEVICE_ANNCE Signal gesetzt
            // ZUSÄTZLICH: Prüfe auch direkt esp_zb_bdb_dev_joined(), falls DEVICE_ANNCE Signal nicht kommt
            // WICHTIG: Kurze Verzögerung nach Network Steering, damit Stack Zeit hat, Join-State zu aktualisieren
            // (verhindert Race Condition zwischen State-Update und Prüfung)
            const uint32_t direct_check_delay_ms = 200;  // 200ms Verzögerung vor direkter Prüfung
            bool direct_check_started = false;
            uint32_t direct_check_start_ms = 0;
            
            while (!pairing_successful && !steering_failed && elapsed_ms < cycle_timeout_ms && (elapsed_ms - cycle_start_ms) < pairing_timeout_ms) {
                // ZUSÄTZLICHE PRÜFUNG: Direkt prüfen, ob Device joined ist (falls DEVICE_ANNCE Signal nicht kommt)
                // Dies ist wichtig, da manche Coordinator das DEVICE_ANNCE Signal nicht senden,
                // aber das Device trotzdem erfolgreich joined ist
                // HINWEIS: Warte erst nach Network Steering, bevor direkte Prüfung startet
                if (steering_successful && !direct_check_started) {
                    direct_check_started = true;
                    direct_check_start_ms = elapsed_ms;
                    ESP_LOGI(TAG, "        → Warte %d ms vor direkter Join-Prüfung (State-Update-Zeit)...", direct_check_delay_ms);
                }
                
                // Direkte Prüfung nur nach Verzögerung und wenn Network Steering erfolgreich war
                if (direct_check_started && (elapsed_ms - direct_check_start_ms) >= direct_check_delay_ms) {
                    zigbee_network_info_t join_info = zigbee_read_network_info_locked();
                    bool has_valid_network_addr = zigbee_network_info_valid(&join_info);
                    
                    if (join_info.joined && !zigbee_rtc.joined && has_valid_network_addr) {
                        // Device ist joined, aber pairing_successful wurde noch nicht gesetzt
                        // (DEVICE_ANNCE Signal kam nicht, aber Join war erfolgreich)
                        ESP_LOGI(TAG, "        → Device ist joined (direkte Prüfung), aber DEVICE_ANNCE Signal kam nicht");
                        ESP_LOGI(TAG, "        → Network Address: 0x%04X, PAN ID: 0x%04X",
                                 join_info.network_addr, join_info.pan_id);
                        ESP_LOGI(TAG, "        → Setze Pairing-Status manuell...");
                        
                        // Status manuell setzen (wie im DEVICE_ANNCE Handler)
                        pairing_successful = true;
                        first_pairing_after_join = true;
                        zigbee_stack_device_annce_received = true;  // Join ohne ZDO-Signal
                        
                        zigbee_update_rtc_from_stack();
                        zigbee_send_device_annce_if_needed("pairing_direct_check");
                        
                        if (zigbee_config_save_to_nvs()) {
                            ESP_LOGI(TAG, "        → ZigBee-Config erfolgreich in NVS gespeichert (manuell nach direkter Prüfung)");
                        } else {
                            ESP_LOGE(TAG, "        → FEHLER: ZigBee-Config konnte nicht in NVS gespeichert werden!");
                        }
                        
                        break;  // Pairing erfolgreich, aus Schleife ausbrechen
                    }
                }
                
                vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
                elapsed_ms += poll_interval_ms;
            }
            
            if (steering_failed) {
                ESP_LOGW(TAG, "        → Network Steering fehlgeschlagen (Versuch %d, nach %d ms)", 
                         steering_attempt + 1, elapsed_ms - cycle_start_ms);
                steering_attempt++;
                // Weiter mit Retry, falls noch Versuche übrig sind
                continue;
            }
            
            if (pairing_successful) {
                ESP_LOGI(TAG, "        → Pairing erfolgreich (Versuch %d, nach %d ms)", 
                         steering_attempt + 1, elapsed_ms - cycle_start_ms);
                pairing_completed = true;
                break;
            }
            
            // Timeout (nicht Steering FAIL) -> kein Retry
            ESP_LOGE(TAG, "        → Pairing-Timeout (Versuch %d, nach %d ms)", 
                     steering_attempt + 1, elapsed_ms - cycle_start_ms);
            zigbee_pairing_in_progress = false;
            zigbee_restore_sleepy_rx_on_when_idle();
            return TRANSFER_STATUS_CONNECTION_FAILED;
        }

        zigbee_pairing_in_progress = false;

        if (!pairing_completed) {
            ESP_LOGE(TAG, "        → Pairing fehlgeschlagen nach %d Versuchen (Gesamt-Zeit: %d ms)", 
                     steering_attempt, elapsed_ms);
            
            // WICHTIG: Prüfe nochmal, ob das Device zwischenzeitlich erfolgreich gejoint ist
            // (kann passieren, wenn der Stack im Hintergrund weiterarbeitet und später erfolgreich ist)
            // Dies ist besonders wichtig für factory-new Devices, die während des gleichen Wake-ups joinen
            zigbee_network_info_t join_info = zigbee_read_network_info_locked();
            
            if (join_info.joined && zigbee_network_info_valid(&join_info)) {
                ESP_LOGI(TAG, "        → WICHTIG: Device ist jetzt doch joined (zwischenzeitlich erfolgreich gejoint)!");
                ESP_LOGI(TAG, "        → Network Address: 0x%04X, PAN ID: 0x%04X",
                         join_info.network_addr, join_info.pan_id);
                ESP_LOGI(TAG, "        → Setze Pairing-Status nachträglich...");
                
                // Status nachträglich setzen (wie im DEVICE_ANNCE Handler)
                pairing_successful = true;
                first_pairing_after_join = true;  // Flag setzen für Interview-Wartezeit
                
                zigbee_update_rtc_from_stack();
                zigbee_send_device_annce_if_needed("pairing_timeout_fallback");
                
                if (zigbee_config_save_to_nvs()) {
                    ESP_LOGI(TAG, "        → ZigBee-Config erfolgreich in NVS gespeichert (nachträglich nach fehlgeschlagenem Pairing)");
                } else {
                    ESP_LOGE(TAG, "        → FEHLER: ZigBee-Config konnte nicht in NVS gespeichert werden!");
                }
                
                // Pairing war erfolgreich (nachträglich erkannt)
                pairing_completed = true;
            } else {
                // Device ist wirklich nicht joined
                zigbee_pairing_in_progress = false;
                zigbee_restore_sleepy_rx_on_when_idle();
                return TRANSFER_STATUS_CONNECTION_FAILED;
            }
        }
    } else {
        // Rejoin: zuerst ZDO-Rejoin (kein Scan), bei Fehler Network Steering
        ESP_LOGI(TAG, "        → Device ist nicht factory-new, aber nicht joined → Starte Rejoin...");
        zigbee_rejoin_in_progress = true;

        uint32_t steering_attempt = 0;
        bool rejoin_completed = false;
        
        while (steering_attempt <= steering_retry_count && !rejoin_completed && elapsed_ms < cycle_timeout_ms) {
            // Flags zurücksetzen für neuen Versuch
            rejoin_successful = false;
            steering_failed = false;
            steering_successful = false;
            device_rebooted_during_rejoin = false;
            
            if (steering_attempt > 0) {
                ESP_LOGI(TAG, "        → Retry-Versuch %d/%d (nach %d ms Wartezeit)...", 
                         steering_attempt, steering_retry_count, steering_retry_timer_ms);
                vTaskDelay(pdMS_TO_TICKS(steering_retry_timer_ms));
                elapsed_ms += steering_retry_timer_ms;
            }
            
            if (ZIGBEE_RTC_HAS_DIRECT_REJOIN_CTX()) {
                uint32_t direct_elapsed_ms = 0;
                if (zigbee_try_direct_bdb_rejoin(ZIGBEE_DIRECT_REJOIN_TIMEOUT_MS, &direct_elapsed_ms)) {
                    ESP_LOGI(TAG, "        → Rejoin erfolgreich (direkter Rejoin, %lu ms)",
                             (unsigned long)direct_elapsed_ms);
                    zigbee_finalize_rejoin_success();
                    rejoin_completed = true;
                    zigbee_rejoin_in_progress = false;
                    break;
                }
                elapsed_ms += direct_elapsed_ms;
                if (elapsed_ms >= cycle_timeout_ms) {
                    break;
                }
                uint32_t post_fail_wait_ms = 0;
                if (zigbee_wait_stack_ready_after_failed_direct_rejoin(
                        ZIGBEE_DIRECT_REJOIN_POST_FAIL_WAIT_MS, &post_fail_wait_ms)) {
                    ESP_LOGI(TAG, "        → Rejoin erfolgreich nach INITIALIZATION-Timeout (+%lu ms)",
                             (unsigned long)post_fail_wait_ms);
                    zigbee_finalize_rejoin_success();
                    rejoin_completed = true;
                    zigbee_rejoin_in_progress = false;
                    elapsed_ms += post_fail_wait_ms;
                    break;
                }
                elapsed_ms += post_fail_wait_ms;
                if (elapsed_ms >= cycle_timeout_ms) {
                    break;
                }
            } else {
                ESP_LOGI(TAG, "        → Kein gueltiger RTC-Kontext (Ch/PAN) → Network Steering");
            }

            zigbee_enable_rx_before_steering();
            esp_err_t comm_err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            if (comm_err != ESP_OK) {
                ESP_LOGE(TAG, "        → Fehler beim Starten von Network Steering (Rejoin, Versuch %d): %s", 
                         steering_attempt + 1, esp_err_to_name(comm_err));
                steering_attempt++;
                continue;  // Nächster Retry-Versuch
            }
            
            ESP_LOGI(TAG, "        → Network Steering gestartet (Rejoin, Versuch %d/%d, Timeout: %d ms)", 
                     steering_attempt + 1, steering_retry_count , rejoin_timeout_ms);
            cycle_start_ms = elapsed_ms;
            
            // Warte auf Network Steering Erfolg (mit Timeout)
            // WICHTIG: 500ms Poll-Intervall gibt dem Stack genug Zeit, Signale zu verarbeiten
            // (200ms war zu kurz und könnte Race Conditions verursachen)
            const uint32_t steering_poll_interval_ms = 500;  // 500ms für zuverlässige Signal-Verarbeitung (konsistent mit ZIGBEE_STEERING_POLL_INTERVAL_MS)
            uint32_t steering_wait_start_ms = elapsed_ms;
            while (!steering_successful && !steering_failed && elapsed_ms < cycle_timeout_ms && 
                   (elapsed_ms - steering_wait_start_ms) < ZIGBEE_NETWORK_DISCOVERY_MS) {
                uint32_t reboot_poll_ms = 0;
                if (zigbee_try_handle_reboot_in_steering_wait(false, &reboot_poll_ms)) {
                    elapsed_ms += reboot_poll_ms;
                    break;
                }
                elapsed_ms += reboot_poll_ms;
                vTaskDelay(pdMS_TO_TICKS(steering_poll_interval_ms));
                elapsed_ms += steering_poll_interval_ms;
                if (steering_failed) {
                    ESP_LOGI(TAG, "        → Network Steering fehlgeschlagen erkannt (nach %d ms) → Breche sofort ab", 
                             elapsed_ms - steering_wait_start_ms);
                    break;
                }
            }
            
            if (rejoin_successful) {
                zigbee_finalize_rejoin_success();
                rejoin_completed = true;
                zigbee_rejoin_in_progress = false;
                break;
            }
            
            if (steering_successful) {
                ESP_LOGI(TAG, "        → STEERING OK - warte %d ms (Stack-Stabilisierung vor Join-Pruefung)",
                         ZIGBEE_STEERING_TO_ASSOCIATION_DELAY_MS);
                vTaskDelay(pdMS_TO_TICKS(ZIGBEE_STEERING_TO_ASSOCIATION_DELAY_MS));
                elapsed_ms += ZIGBEE_STEERING_TO_ASSOCIATION_DELAY_MS;
            }
            
            // Warte auf Rejoin-Erfolg (mit Timeout) oder Steering-Fehler
            // HINWEIS: rejoin_successful wird im DEVICE_ANNCE Signal gesetzt
            // ZUSÄTZLICH: Prüfe auch direkt esp_zb_bdb_dev_joined(), falls DEVICE_ANNCE Signal nicht kommt
            // WICHTIG: Kurze Verzögerung nach Network Steering, damit Stack Zeit hat, Join-State zu aktualisieren
            // (verhindert Race Condition zwischen State-Update und Prüfung)
            const uint32_t direct_check_delay_ms = 200;  // 200ms Verzögerung vor direkter Prüfung
            bool direct_check_started = false;
            uint32_t direct_check_start_ms = 0;
            
            // SICHERHEIT: Prüfe, ob Network Steering überhaupt gestartet wurde (nicht fehlgeschlagen, aber auch nicht erfolgreich)
            // Wenn Network Steering nach einem Timeout weder erfolgreich noch fehlgeschlagen ist, könnte der Stack hängen
            bool network_steering_timeout_reached = false;
            uint32_t network_steering_timeout_ms = 5000;  // 5 Sekunden zusätzlicher Timeout für Network Steering (falls Stack hängt)
            uint32_t network_steering_start_ms = elapsed_ms;
            
            while (!rejoin_successful && !steering_failed && elapsed_ms < cycle_timeout_ms && (elapsed_ms - cycle_start_ms) < rejoin_timeout_ms) {
                // WICHTIG: Wenn DEVICE_REBOOT (ESP_OK) während eines manuellen Rejoin-Versuchs kommt,
                // wurde der Stack neu initialisiert und startet automatisch einen Rejoin
                // In diesem Fall sollten wir auf den automatischen Rejoin warten (ähnlich wie beim automatischen Rejoin am Anfang)
                if (device_rebooted_during_rejoin) {
                    ESP_LOGI(TAG, "        → DEVICE_REBOOT waehrend Rejoin erkannt → Poll Join...");
                    uint32_t poll_elapsed_ms = 0;
                    if (zigbee_poll_joined_after_reboot(ZIGBEE_AUTO_REJOIN_WAIT_TIMEOUT_MS, false,
                                                        &poll_elapsed_ms)) {
                        elapsed_ms += poll_elapsed_ms;
                        device_rebooted_during_rejoin = false;
                        break;
                    }
                    elapsed_ms += poll_elapsed_ms;
                    device_rebooted_during_rejoin = false;
                    ESP_LOGW(TAG, "        → Rejoin nach DEVICE_REBOOT nicht innerhalb %lu ms",
                             (unsigned long)poll_elapsed_ms);
                }
                
                // SICHERHEIT: Prüfe, ob Network Steering nach langer Zeit weder erfolgreich noch fehlgeschlagen ist
                // Dies könnte auf einen hängenden Stack hindeuten
                if (!steering_successful && !steering_failed && !device_rebooted_during_rejoin && 
                    (elapsed_ms - network_steering_start_ms) >= network_steering_timeout_ms && !network_steering_timeout_reached) {
                    network_steering_timeout_reached = true;
                    ESP_LOGW(TAG, "        → WARNUNG: Network Steering weder erfolgreich noch fehlgeschlagen nach %d ms (Stack könnte hängen?)", 
                             elapsed_ms - network_steering_start_ms);
                    ESP_LOGW(TAG, "        → Breche Rejoin-Versuch ab und versuche Retry...");
                    steering_failed = true;  // Setze steering_failed, um Schleife zu beenden
                    break;
                }
                
                // ZUSÄTZLICHE PRÜFUNG: Direkt prüfen, ob Device joined ist (falls DEVICE_ANNCE Signal nicht kommt)
                // Dies ist wichtig, da manche Coordinator das DEVICE_ANNCE Signal nicht senden,
                // aber das Device trotzdem erfolgreich rejoined ist
                // HINWEIS: Bei Rejoin kann zigbee_rtc.joined bereits true sein (aus NVS geladen),
                // daher prüfen wir nur, ob der Stack joined ist und rejoin_successful noch nicht gesetzt wurde
                // HINWEIS: Warte erst nach Network Steering, bevor direkte Prüfung startet
                if (steering_successful && !direct_check_started) {
                    direct_check_started = true;
                    direct_check_start_ms = elapsed_ms;
                    ESP_LOGI(TAG, "        → Warte %d ms vor direkter Rejoin-Prüfung (State-Update-Zeit)...", direct_check_delay_ms);
                }
                
                // Direkte Prüfung nur nach Verzögerung und wenn Network Steering erfolgreich war
                // OPTIMIERUNG: Auch prüfen, wenn Network Steering nicht erfolgreich war, aber genug Zeit vergangen ist
                // (Fallback für den Fall, dass Network Steering erfolgreich war, aber steering_successful nicht gesetzt wurde)
                bool should_check = false;
                if (steering_successful && direct_check_started && (elapsed_ms - direct_check_start_ms) >= direct_check_delay_ms) {
                    should_check = true;
                } else if (!steering_successful && (elapsed_ms - cycle_start_ms) >= 3000) {
                    // Fallback: Prüfe auch, wenn Network Steering nicht erfolgreich war, aber 3 Sekunden vergangen sind
                    // (könnte sein, dass Network Steering erfolgreich war, aber steering_successful nicht gesetzt wurde)
                    if (!direct_check_started) {
                        direct_check_started = true;
                        direct_check_start_ms = elapsed_ms;
                        ESP_LOGI(TAG, "        → Fallback: Prüfe Rejoin-Status (Network Steering nicht erfolgreich, aber %d ms vergangen)", 
                                 elapsed_ms - cycle_start_ms);
                    }
                    if ((elapsed_ms - direct_check_start_ms) >= direct_check_delay_ms) {
                        should_check = true;
                    }
                }
                
                if (should_check) {
                    zigbee_network_info_t join_info = zigbee_read_network_info_locked();
                    bool has_valid_network = zigbee_network_info_valid(&join_info);
                    
                    if (join_info.joined && !rejoin_successful && has_valid_network) {
                        // Device ist rejoined, aber rejoin_successful wurde noch nicht gesetzt
                        // (DEVICE_ANNCE Signal kam nicht, aber Rejoin war erfolgreich)
                        ESP_LOGI(TAG, "        → Device ist rejoined (direkte Prüfung), aber DEVICE_ANNCE Signal kam nicht");
                        ESP_LOGI(TAG, "        → Network Address: 0x%04X, PAN ID: 0x%04X",
                                 join_info.network_addr, join_info.pan_id);
                        ESP_LOGI(TAG, "        → Setze Rejoin-Status manuell...");
                        
                        rejoin_successful = true;
                        zigbee_update_rtc_from_stack();
                        zigbee_maybe_send_device_annce_on_rejoin("rejoin_direct_check");
                        if (zigbee_config_save_to_nvs()) {
                            ESP_LOGI(TAG, "        → ZigBee-Config erfolgreich in NVS gespeichert (manuell nach direkter Prüfung, Rejoin)");
                        } else {
                            ESP_LOGW(TAG, "        → Warnung: ZigBee-Config konnte nicht in NVS gespeichert werden (Rejoin)");
                        }
                        break;
                    }
                }
                
                vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
                elapsed_ms += poll_interval_ms;
                
                // Logging alle 5 Sekunden, um Fortschritt zu zeigen (verhindert "hängt"-Eindruck)
                if ((elapsed_ms - cycle_start_ms) % 5000 == 0 && (elapsed_ms - cycle_start_ms) > 0) {
                    ESP_LOGI(TAG, "        → Warte auf Rejoin... (%d ms / %d ms Timeout, Versuch %d/%d)", 
                             elapsed_ms - cycle_start_ms, rejoin_timeout_ms, steering_attempt + 1, steering_retry_count + 1);
                }
            }
            
            if (steering_failed) {
                ESP_LOGW(TAG, "        → Network Steering fehlgeschlagen (Rejoin, Versuch %d, nach %d ms)", 
                         steering_attempt + 1, elapsed_ms - cycle_start_ms);
                steering_attempt++;
                // Weiter mit Retry, falls noch Versuche übrig sind
                continue;
            }
            
            if (rejoin_successful) {
                ESP_LOGI(TAG, "        → Rejoin erfolgreich (Versuch %d, nach %d ms)", 
                         steering_attempt + 1, elapsed_ms - cycle_start_ms);
                zigbee_finalize_rejoin_success();
                rejoin_completed = true;
                zigbee_rejoin_in_progress = false;
                break;
            }
            
            // Timeout (nicht Steering FAIL) -> Retry, falls noch Versuche übrig sind
            ESP_LOGE(TAG, "        → Rejoin-Timeout (Versuch %d, nach %d ms)", 
                     steering_attempt + 1, elapsed_ms - cycle_start_ms);
            steering_attempt++;
            // Weiter mit Retry, falls noch Versuche übrig sind (steering_attempt <= steering_retry_count)
            if (steering_attempt <= steering_retry_count) {
                ESP_LOGI(TAG, "        → Retry-Versuch %d/%d wird gestartet...", steering_attempt, steering_retry_count);
                continue;  // Nächster Retry-Versuch
            } else {
                // Keine weiteren Versuche mehr
                ESP_LOGE(TAG, "        → Keine weiteren Retry-Versuche mehr (max: %d)", steering_retry_count);
                zigbee_rejoin_in_progress = false;
                zigbee_restore_sleepy_rx_on_when_idle();
                return TRANSFER_STATUS_CONNECTION_FAILED;
            }
        }

        zigbee_rejoin_in_progress = false;

        if (!rejoin_completed) {
            ESP_LOGE(TAG, "        → Rejoin fehlgeschlagen nach %d Versuchen (Gesamt-Zeit: %d ms)", 
                     steering_attempt, elapsed_ms);
            zigbee_restore_sleepy_rx_on_when_idle();
            return TRANSFER_STATUS_CONNECTION_FAILED;
        }
    }
    
    // Prüfe Gesamt-Timeout
    if (elapsed_ms >= cycle_timeout_ms) {
        ESP_LOGE(TAG, "        → Gesamt-Timeout erreicht (nach %d ms)", elapsed_ms);
        if (!is_factory_new) {
            zigbee_restore_sleepy_rx_on_when_idle();
        }
        return TRANSFER_STATUS_CONNECTION_FAILED;
    }
    
    return TRANSFER_STATUS_OK;
}

// ============================================
// Helper: Einzelnes Attribut explizit reporten (Branch zigbee_reporting_fix)
// ============================================
// Sendet einen "Report Attributes"-Command an alle gebundenen Empfaenger
// (address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT). Der Stack
// loest den Eintrag ueber die Binding-Tabelle auf, kein expliziter Coordinator
// noetig. Reports werden vom Server zum Client geschickt -> DIRECTION_TO_CLI.
// Belt-and-Braces: explizite report_attr_cmd_req (nicht set_attribute_val – TZ-292).
// Cluster-RAM wird in transfer_zigbee_prepare_cluster_attrs() aktualisiert; der Stack
// liest die beim Register gebundenen Pointer lazy. set_attribute_val wuerde REPORTABLE
// setzen und ungeplante Reports ausloesen.
//
// Rueckgabe: true bei ESP_OK von esp_zb_zcl_report_attr_cmd_req.
// Aufrufer muss esp_zb_lock_acquire halten (Zigbee-Stack-API vom Nicht-Zigbee-Task).
static bool send_attribute_report_locked(uint16_t cluster_id, uint16_t attr_id) {
    esp_zb_zcl_report_attr_cmd_t cmd = {};
    cmd.zcl_basic_cmd.src_endpoint = ZIGBEE_ENDPOINT_ID;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;  // bind-basiert
    cmd.clusterID = cluster_id;
    cmd.attributeID = attr_id;
    cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;  // Server -> Client (unsolicited Report)

    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  → report_attr cluster=0x%04X attr=0x%04X fehlgeschlagen: %s",
                 cluster_id, attr_id, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool send_attribute_report(uint16_t cluster_id, uint16_t attr_id) {
    esp_zb_lock_acquire(portMAX_DELAY);
    bool ok = send_attribute_report_locked(cluster_id, attr_id);
    esp_zb_lock_release();
    return ok;
}

transfer_status_t transfer_zigbee_send_data(const transfer_data_t* data) {
    if (data == nullptr) {
        ESP_LOGE(TAG, "transfer_zigbee_send_data: data ist NULL");
        return TRANSFER_STATUS_UNKNOWN_ERROR;
    }
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ZigBee-Datenübertragung");
    ESP_LOGI(TAG, "========================================");
    
    // [1/4] Warte auf Stack-Initialisierung (mit Timeout)
    if (!zigbee_initialized) {
        ESP_LOGI(TAG, "  [1/4] Warte auf ZigBee-Stack Initialisierung...");
        const uint32_t timeout_ms = ZIGBEE_INIT_TIMEOUT_MS;
        const uint32_t poll_interval_ms = ZIGBEE_INIT_POLL_INTERVAL_MS;
        uint32_t elapsed_ms = 0;
        
        while (!zigbee_initialized && elapsed_ms < timeout_ms) {
            vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
            elapsed_ms += poll_interval_ms;
        }
        
        if (!zigbee_initialized) {
            ESP_LOGE(TAG, "        → Timeout: ZigBee-Stack nicht initialisiert (nach %d ms)", timeout_ms);
        return TRANSFER_STATUS_INIT_FAILED;
    }
    
        ESP_LOGI(TAG, "        → ZigBee-Stack initialisiert (nach %d ms)", elapsed_ms);
    } else {
        ESP_LOGI(TAG, "  [1/4] ZigBee-Stack bereits initialisiert");
    }
    
    // Cluster-RAM: idempotent (auch in transfer.cpp vor init); einmal pro send_data
    transfer_zigbee_prepare_cluster_attrs(data);

    // [2/4] Stelle sicher, dass Device mit Netzwerk verbunden ist (Pairing/Rejoin)
    ESP_LOGI(TAG, "  [2/4] Stelle sicher, dass Device mit ZigBee-Netzwerk verbunden ist...");
    
    // WICHTIG: Prüfe, ob Device bereits joined war (vor ensure_joined) - nur für Logging
    bool was_already_joined_in_rtc = zigbee_rtc.joined && ZIGBEE_IS_NETWORK_ADDR_VALID(zigbee_rtc.network_addr);
    bool was_joined_in_stack_before = zigbee_read_network_info_locked().joined;
    
    transfer_status_t join_status = transfer_zigbee_ensure_joined();
    if (join_status != TRANSFER_STATUS_OK) {
        ESP_LOGE(TAG, "        → Fehler beim Verbinden mit ZigBee-Netzwerk");
        ESP_LOGW(TAG, "        → Zeit-Synchronisation wird übersprungen (Device nicht verbunden)");
        return join_status;
    }
    ESP_LOGI(TAG, "        → Device ist mit ZigBee-Netzwerk verbunden");
    zigbee_refresh_parent_link_diagnostics(false);

    // Logging: Art der Verbindung (für Diagnose)
    bool rejoin_just_happened = was_already_joined_in_rtc && !was_joined_in_stack_before
        && zigbee_read_network_info_locked().joined;
    if (rejoin_just_happened) {
        ESP_LOGI(TAG, "        → Rejoin erkannt → Stack sendet Reports automatisch bei Wertänderung");
    } else if (was_already_joined_in_rtc && was_joined_in_stack_before) {
        ESP_LOGI(TAG, "        → Device war bereits verbunden → Stack sendet Reports automatisch");
    } else if (first_pairing_after_join) {
        ESP_LOGI(TAG, "        → Erstes Pairing erkannt → Stack sendet Reports nach Z2M-Configure");
    } else {
        ESP_LOGI(TAG, "        → Normale Verbindung → Stack sendet Reports automatisch");
    }

    // [3/4] Cluster-RAM (prepare_cluster_attrs) + explizite Reports – kein set_attribute_val (TZ-292)
    ESP_LOGI(TAG, "  [3/4] Sende Daten:");
    ESP_LOGI(TAG, "        → pulse_counter: %lu", data->pulse_counter);
    ESP_LOGI(TAG, "        → battery_percent: %.1f%%", data->battery_percent);
    ESP_LOGI(TAG, "        → battery_voltage: %.2fV", data->battery_voltage);
    ESP_LOGI(TAG, "        → firmware_version: %s", data->firmware_version ? data->firmware_version : "N/A");
    ESP_LOGI(TAG, "        → CurrentSummationDelivered (Cluster-RAM): %lu (low: %lu, high: %u)",
             data->pulse_counter, current_summation_delivered.low, current_summation_delivered.high);

    {
        const bool explicit_battery = first_pairing_after_join
            || (ZIGBEE_EXPLICIT_BATTERY_REPORT_ON_REJOIN != 0);
        bool rep_summ = false;
        bool rep_pct = false;
        bool rep_v = false;
        bool rep_alarm = false;

        esp_zb_lock_acquire(portMAX_DELAY);
        rep_summ = send_attribute_report_locked(
            ZIGBEE_CLUSTER_METERING, ZIGBEE_ATTR_METERING_CURRENT_SUMMATION_DELIVERED);
        if (explicit_battery) {
            rep_pct = send_attribute_report_locked(
                ZIGBEE_CLUSTER_BATTERY, ZIGBEE_ATTR_BATTERY_PERCENT);
            if (data->battery_voltage > 0.0f) {
                rep_v = send_attribute_report_locked(
                    ZIGBEE_CLUSTER_BATTERY, ZIGBEE_ATTR_BATTERY_VOLTAGE);
                rep_alarm = send_attribute_report_locked(
                    ZIGBEE_CLUSTER_BATTERY, ZIGBEE_ATTR_BATTERY_ALARM_STATE);
            }
        }
        esp_zb_lock_release();

        ESP_LOGI(TAG, "        → Explizite Reports: summ=%s pct=%s v=%s alarm=%s",
                 rep_summ ? "ok" : "fail",
                 explicit_battery ? (rep_pct ? "ok" : "fail") : "skip",
                 (explicit_battery && data->battery_voltage > 0.0f) ? (rep_v ? "ok" : "fail") : "skip",
                 (explicit_battery && data->battery_voltage > 0.0f) ? (rep_alarm ? "ok" : "fail") : "skip");
    }
    
    // [3.5/4] Erstes Pairing: DEVICE_ANNCE + Post-Announce-Fenster (statt fix 90 s)
    if (first_pairing_after_join) {
        zigbee_wait_for_first_pairing_interview();
        first_pairing_after_join = false;
    }
    
    // [4/4] Zeit-Synchronisation mit Coordinator
    ESP_LOGI(TAG, "  [4/4] Synchronisiere Zeit mit Coordinator...");
    if (transfer_zigbee_sync_time()) {
        ESP_LOGI(TAG, "        → Zeit-Synchronisation Request gesendet (max %d ms auf Response)",
                 ZIGBEE_TIME_SYNC_WAIT_MS);
        zigbee_wait_for_time_sync_response();
        if (zigbee_time_sync_response_received) {
            ESP_LOGI(TAG, "        → Zeit-Synchronisation erfolgreich");
        } else {
            ESP_LOGW(TAG, "        → Zeit-Synchronisation: keine Antwort innerhalb %d ms (nicht kritisch)",
                     ZIGBEE_TIME_SYNC_WAIT_MS);
        }
    } else {
        ESP_LOGW(TAG, "        → Zeit-Synchronisation fehlgeschlagen (Request nicht gesendet, nicht kritisch)");
    }
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ZigBee-Datenübertragung abgeschlossen");
    ESP_LOGI(TAG, "========================================");
    
    return TRANSFER_STATUS_OK;
}

bool transfer_zigbee_sync_time(void) {
    if (!zigbee_initialized) {
        ESP_LOGE(TAG, "transfer_zigbee_sync_time: ZigBee nicht initialisiert");
        return false;
    }
    
    if (!zigbee_read_network_info_locked().joined) {
        ESP_LOGW(TAG, "transfer_zigbee_sync_time: Device nicht mit Netzwerk verbunden");
        return false;
    }
    
    ESP_LOGI(TAG, "transfer_zigbee_sync_time: Hole Zeit vom Coordinator...");
    zigbee_time_sync_response_received = false;
    
    // Coordinator Adresse: Normalerweise 0x0000, aber verwende zigbee_rtc.coord_addr falls gesetzt
    uint16_t coordinator_addr = (zigbee_rtc.coord_addr != ZIGBEE_DEFAULT_COORD_ADDR) 
                                ? zigbee_rtc.coord_addr 
                                : 0x0000;  // Standard Coordinator Adresse
    
    // Read Attribute Command für Time Cluster, Time Attribut (0x0000)
    esp_zb_zcl_read_attr_cmd_t read_cmd = {0};
    
    // Basis-ZCL-Command-Konfiguration
    read_cmd.zcl_basic_cmd.dst_addr_u.addr_short = coordinator_addr;
    read_cmd.zcl_basic_cmd.src_endpoint = ZIGBEE_ENDPOINT_ID;
    read_cmd.zcl_basic_cmd.dst_endpoint = 1;  // Coordinator Endpoint (Standard: 1)
    read_cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;  // 16-bit Adresse + Endpoint
    read_cmd.clusterID = ZIGBEE_CLUSTER_TIME;  // Time Cluster (0x000A)
    
    // Richtung über Bit-Feld setzen: Client → Server (Device → Coordinator)
    read_cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    
    // Attribute-Liste: Nur Time Attribut (0x0000) lesen
    // HINWEIS: attr_field ist ein uint16_t* Pointer, attr_number ist die Anzahl
    static uint16_t attr_ids[1] = {ZIGBEE_ATTR_TIME_TIME};  // Time Attribut (0x0000)
    read_cmd.attr_field = attr_ids;
    read_cmd.attr_number = 1;
    
    // Thread-Safety: Zigbee Stack Lock
    esp_zb_lock_acquire(portMAX_DELAY);
    uint8_t seq_num = esp_zb_zcl_read_attr_cmd_req(&read_cmd);
    esp_zb_lock_release();
    
    if (seq_num == 0) {
        ESP_LOGW(TAG, "  → Read Attribute Request fehlgeschlagen (Time Cluster)");
        return false;
    }
    
    ESP_LOGI(TAG, "  → Read Attribute Request gesendet (Time Cluster, Seq: %u, Coordinator: 0x%04X)", 
             seq_num, coordinator_addr);
    ESP_LOGI(TAG, "  → Warte auf Antwort vom Coordinator...");
    
    // HINWEIS: Die Response-Verarbeitung muss noch implementiert werden
    // Die Response wird über einen Callback oder Signal-Handler verarbeitet
    // TODO: Response-Handler für Read Attribute Response implementieren
    
    return true;
}

void transfer_zigbee_deinit(void) {
    if (!zigbee_initialized) {
        return;
    }
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ZigBee beenden (Main Loop stoppen)");
    ESP_LOGI(TAG, "========================================");
    
    // ZigBee Main Loop Task beenden
    if (zigbee_main_task_handle != NULL) {
        ESP_LOGI(TAG, "  [1/2] ZigBee Main Loop Task wird beendet...");
        zigbee_initialized = false;  // Signal zum Beenden
        vTaskDelay(pdMS_TO_TICKS(ZIGBEE_DEINIT_DELAY_MS));  // Warten, bis Task beendet ist
        zigbee_main_task_handle = NULL;
        ESP_LOGI(TAG, "        → Task beendet");
    }
    
    // Kein esp_zb_deinit() (nicht in esp-zigbee-lib) – nur Main-Loop-Task stoppen.
    // Nach Factory-Reset folgt ohnehin Reboot (Web-UX); danach frischer Stack-Start.
    ESP_LOGI(TAG, "  [2/2] ZigBee Main Loop gestoppt (kein vollstaendiger Stack-Teardown)");
    
    ESP_LOGI(TAG, "========================================");
    
    zigbee_initialized = false;
    stack_ready_signal_received = false;  // Flag zurücksetzen
}

// ============================================
// Wrapper-Funktionen für Web-Interface
// ============================================

bool transfer_zigbee_get_status_json(char* buffer, size_t buffer_size) {
    if (buffer == NULL || buffer_size < 512) {
        ESP_LOGE(TAG, "transfer_zigbee_get_status_json: Buffer zu klein oder NULL");
        return false;
    }
    
    // ZigBee-Status ermitteln über Wrapper-Funktionen
    // Diese prüfen automatisch den Stack-Status (wenn initialisiert) oder fallen auf zigbee_rtc zurück
    bool is_joined = transfer_zigbee_is_joined();
    bool is_factory_new = transfer_zigbee_is_factory_new();
    
    // JSON-Response erstellen
    int written = snprintf(buffer, buffer_size,
        "{"
        "\"status\":\"%s\","
        "\"factory_new\":%s,"
        "\"joined\":%s,"
        "\"network_addr\":\"0x%04X\","
        "\"pan_id\":\"0x%04X\","
        "\"channel\":%d,"
        "\"extended_addr\":\"0x%016llX\""
        "}",
        is_factory_new ? "factory-new" : (is_joined ? "joined" : "not-joined"),
        is_factory_new ? "true" : "false",
        is_joined ? "true" : "false",
        zigbee_rtc.network_addr,
        zigbee_rtc.pan_id,
        zigbee_rtc.channel,
        (unsigned long long)zigbee_rtc.extended_addr
    );
    
    if (written < 0 || (size_t)written >= buffer_size) {
        ESP_LOGE(TAG, "transfer_zigbee_get_status_json: Fehler beim Erstellen des JSON-Strings");
        return false;
    }
    
    return true;
}

bool transfer_zigbee_factory_reset(const char* transfer_mode) {
    if (transfer_mode == NULL) {
        ESP_LOGE(TAG, "transfer_zigbee_factory_reset: transfer_mode ist NULL");
        return false;
    }
    
    // Prüfe, ob bereits ein Factory-Reset läuft
    if (factory_reset_in_progress) {
        ESP_LOGW(TAG, "transfer_zigbee_factory_reset: Factory-Reset läuft bereits");
        return false;
    }
    
    factory_reset_in_progress = true;  // Flag setzen
    ESP_LOGI(TAG, "ZigBee Factory-Reset wird durchgeführt...");
    
    // 1. NVS-Namespace "zigbee_config" löschen
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(ZIGBEE_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_erase_all(nvs_handle);
        if (err == ESP_OK) {
            nvs_commit(nvs_handle);
            ESP_LOGI(TAG, "  → NVS-Namespace '%s' gelöscht", ZIGBEE_NVS_NAMESPACE);
        }
        nvs_close(nvs_handle);
    }
    
    // 2. Partition "zb_storage" löschen
    const esp_partition_t* zb_storage = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "zb_storage");
    if (zb_storage != NULL) {
        err = esp_partition_erase_range(zb_storage, 0, zb_storage->size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  → Partition 'zb_storage' gelöscht");
        } else {
            ESP_LOGW(TAG, "  → Fehler beim Löschen von 'zb_storage': %s", esp_err_to_name(err));
        }
    }
    
    // 3. Partition "zb_fct" löschen
    const esp_partition_t* zb_fct = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "zb_fct");
    if (zb_fct != NULL) {
        err = esp_partition_erase_range(zb_fct, 0, zb_fct->size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  → Partition 'zb_fct' gelöscht");
        } else {
            ESP_LOGW(TAG, "  → Fehler beim Löschen von 'zb_fct': %s", esp_err_to_name(err));
        }
    }
    
    // 4. zigbee_rtc auf Default-Werte setzen
    zigbee_rtc.joined = false;
    zigbee_rtc.network_addr = ZIGBEE_INVALID_NETWORK_ADDR;
    zigbee_rtc.coord_addr = ZIGBEE_DEFAULT_COORD_ADDR;
    zigbee_rtc.pan_id = ZIGBEE_DEFAULT_PAN_ID;
    zigbee_rtc.channel = ZIGBEE_DEFAULT_CHANNEL;
    zigbee_rtc.extended_addr = ZIGBEE_DEFAULT_EXTENDED_ADDR;
    
    ESP_LOGI(TAG, "  → ZigBee Factory-Reset abgeschlossen");
    
    // 5. ZigBee Main Loop stoppen (kein esp_zb_deinit; Reboot ueber Web-UX reicht)
    if (zigbee_initialized) {
        ESP_LOGI(TAG, "  → ZigBee Main Loop wird gestoppt...");
        transfer_zigbee_deinit();
        ESP_LOGI(TAG, "  → Main Loop gestoppt (Stack-Reset erfolgt beim Geraete-Reboot)");
    } else {
        ESP_LOGI(TAG, "  → ZigBee-Stack war nicht initialisiert");
    }
    
    factory_reset_in_progress = false;  // Flag zurücksetzen
    return true;
}

transfer_status_t transfer_zigbee_start_pairing(void) {
    // Prüfe, ob ZigBee-Stack initialisiert ist
    if (!zigbee_initialized) {
        // Stack initialisieren (falls noch nicht geschehen)
        if (!transfer_zigbee_init()) {
            ESP_LOGE(TAG, "transfer_zigbee_start_pairing: ZigBee-Initialisierung fehlgeschlagen");
            return TRANSFER_STATUS_INIT_FAILED;
        }
    }
    
    // Versuche Network Steering zu starten
    esp_err_t comm_err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
    if (comm_err == ESP_OK) {
        ESP_LOGI(TAG, "transfer_zigbee_start_pairing: Network Steering gestartet");
        return TRANSFER_STATUS_OK;
    }
    ESP_LOGE(TAG, "transfer_zigbee_start_pairing: Fehler beim Starten von Network Steering: %s",
             esp_err_to_name(comm_err));
    return TRANSFER_STATUS_CONNECTION_FAILED;
}

bool transfer_zigbee_is_factory_reset_in_progress(void) {
    return factory_reset_in_progress;
}

/**
 * @brief Prüft, ob der ZigBee-Stack initialisiert ist
 * 
 * @return true wenn Stack initialisiert ist, false sonst
 */
bool transfer_zigbee_is_initialized(void) {
    return zigbee_initialized;
}

/**
 * @brief Prüft, ob das Device mit dem ZigBee-Netzwerk verbunden ist
 * 
 * @return true wenn Device joined ist, false sonst
 */
bool transfer_zigbee_is_joined(void) {
    if (zigbee_initialized) {
        return zigbee_read_network_info_locked().joined;
    }
    return zigbee_rtc.joined;
}

/**
 * @brief Prüft, ob das Device factory-new ist (noch nicht gepaart)
 * 
 * @return true wenn Device factory-new ist, false sonst
 */
bool transfer_zigbee_is_factory_new(void) {
    if (zigbee_initialized) {
        return esp_zb_bdb_is_factory_new();
    }
    // Fallback: Wenn nicht joined und keine gültige Network Address → factory-new
    return !zigbee_rtc.joined && !ZIGBEE_IS_NETWORK_ADDR_VALID(zigbee_rtc.network_addr);
}
