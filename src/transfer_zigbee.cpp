#include "transfer_zigbee.h"
#include "zigbee_config.h"
#include "hardware.h"
#include "version.h"

#ifndef ARDUINO
    #include "esp_log.h"
    #include "nvs.h"
    #include "nvs_flash.h"
    #include "esp_zigbee_core.h"
    #include "esp_zigbee_type.h"
    #include "esp_zigbee_cluster.h"
    #include "esp_zigbee_endpoint.h"
    #include "esp_zigbee_attribute.h"
    #include "bdb/esp_zigbee_bdb_commissioning.h"  // Für esp_zb_bdb_start_top_level_commissioning, esp_zb_bdb_is_factory_new, esp_zb_bdb_dev_joined
    #include "nwk/esp_zigbee_nwk.h"  // Für ESP_ZB_DEVICE_TYPE_ED
    #include "zcl/esp_zigbee_zcl_common.h"  // Für ESP_ZB_AF_HA_PROFILE_ID, ESP_ZB_HA_CUSTOM_ATTR_DEVICE_ID
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
    #include "esp_partition.h"  // Für Factory-Reset (Partitionen löschen)
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
static TaskHandle_t zigbee_main_task_handle = NULL;
static bool factory_reset_in_progress = false;  // Flag: Factory-Reset läuft gerade
static volatile bool stack_ready_signal_received = false;  // Flag: SKIP_STARTUP Signal empfangen
#define ZIGBEE_INIT_TIMEOUT_MS 5000  // 5 Sekunden Timeout für Stack-Initialisierung
#define ZIGBEE_INIT_POLL_INTERVAL_MS 100  // Poll-Intervall für Stack-Initialisierung

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
    
    err = nvs_open(ZIGBEE_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "zigbee_config_load_from_nvs: NVS-Namespace '%s' existiert nicht (noch nicht gepaart) → RTC-RAM auf Default-Werte gesetzt", 
                 ZIGBEE_NVS_NAMESPACE);
        // RTC-RAM ist bereits auf Default-Werte gesetzt
        return true;  // Kein Fehler, einfach noch nicht gepaart
    }
    
    // Lade alle Werte aus NVS
    size_t required_size = sizeof(zigbee_rtc_t);
    err = nvs_get_blob(nvs_handle, ZIGBEE_NVS_KEY_CONFIG, &zigbee_rtc, &required_size);
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
    err = nvs_set_blob(nvs_handle, ZIGBEE_NVS_KEY_CONFIG, &zigbee_rtc, sizeof(zigbee_rtc_t));
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
            break;
            
        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
            ESP_LOGI(TAG, "ZigBee Signal: DEVICE_FIRST_START (Status: %s)", esp_err_to_name(err_status));
            if (err_status == ESP_OK) {
                ESP_LOGI(TAG, "        → Device gestartet (Factory-New: %s)", 
                         esp_zb_bdb_is_factory_new() ? "ja" : "nein");
            }
            break;
            
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            ESP_LOGI(TAG, "ZigBee Signal: DEVICE_REBOOT (Status: %s)", esp_err_to_name(err_status));
            break;
            
        case ESP_ZB_BDB_SIGNAL_STEERING:
            if (err_status == ESP_OK) {
                ESP_LOGI(TAG, "ZigBee Signal: STEERING erfolgreich (Network Discovery)");
                ESP_LOGI(TAG, "        → PAN ID: 0x%04X, Channel: %d, Short Address: 0x%04X",
                         esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
            } else {
                ESP_LOGW(TAG, "ZigBee Signal: STEERING fehlgeschlagen (Status: %s)", esp_err_to_name(err_status));
            }
            break;
            
        case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
            ESP_LOGI(TAG, "ZigBee Signal: DEVICE_ANNCE");
            break;
            
        case ESP_ZB_ZDO_SIGNAL_LEAVE:
            ESP_LOGI(TAG, "ZigBee Signal: LEAVE (Status: %s)", esp_err_to_name(err_status));
            break;
            
        default:
            ESP_LOGI(TAG, "ZigBee Signal: Unknown (Type: 0x%X, Status: %s)", 
                     sig_type, esp_err_to_name(err_status));
            break;
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
        const uint32_t timeout_ms = ZIGBEE_INIT_TIMEOUT_MS;
        const uint32_t poll_interval_ms = ZIGBEE_INIT_POLL_INTERVAL_MS;
        uint32_t elapsed_ms = 0;
        
        while (!stack_ready_signal_received && elapsed_ms < timeout_ms) {
            esp_zb_stack_main_loop_iteration();  // Events verarbeiten (wichtig für Signal-Handler!)
            vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
            elapsed_ms += poll_interval_ms;
        }
        
        if (!stack_ready_signal_received) {
            ESP_LOGE(TAG, "        → Timeout: Stack-Initialisierung nicht abgeschlossen (nach %d ms)", timeout_ms);
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
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,  // Home Automation Profile
        .app_device_id = ESP_ZB_HA_CUSTOM_ATTR_DEVICE_ID,  // Custom Device
        .app_device_version = 0,
    };
    
    // Cluster-Liste erstellen
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    if (cluster_list == NULL) {
        ESP_LOGE(TAG, "Fehler beim Erstellen der Cluster-Liste");
        return NULL;
    }
    
    // Basic Cluster hinzufügen (für Firmware-Version)
    esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(NULL);
    if (basic_cluster == NULL) {
        ESP_LOGE(TAG, "Fehler beim Erstellen des Basic Clusters");
        return NULL;
    }
    esp_err_t err = esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fehler beim Hinzufügen des Basic Clusters: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "  → Basic Cluster hinzugefügt (Endpoint: %d)", ZIGBEE_ENDPOINT_ID);
    
    // Power Configuration Cluster hinzufügen (für Battery)
    esp_zb_attribute_list_t *power_config_cluster = esp_zb_power_config_cluster_create(NULL);
    if (power_config_cluster == NULL) {
        ESP_LOGE(TAG, "Fehler beim Erstellen des Power Configuration Clusters");
        return NULL;
    }
    err = esp_zb_cluster_list_add_power_config_cluster(cluster_list, power_config_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fehler beim Hinzufügen des Power Configuration Clusters: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "  → Power Configuration Cluster hinzugefügt (Endpoint: %d)", ZIGBEE_ENDPOINT_ID);
    
    // Analog Input Cluster hinzufügen (für Pulse Counter)
    esp_zb_attribute_list_t *analog_input_cluster = esp_zb_analog_input_cluster_create(NULL);
    if (analog_input_cluster == NULL) {
        ESP_LOGE(TAG, "Fehler beim Erstellen des Analog Input Clusters");
        return NULL;
    }
    err = esp_zb_cluster_list_add_analog_input_cluster(cluster_list, analog_input_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fehler beim Hinzufügen des Analog Input Clusters: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "  → Analog Input Cluster hinzugefügt (Endpoint: %d)", ZIGBEE_ENDPOINT_ID);
    
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

bool transfer_zigbee_init(void) {
    if (zigbee_initialized) {
        ESP_LOGW(TAG, "transfer_zigbee_init: Bereits initialisiert");
        return true;
    }
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ZigBee-Stack Initialisierung");
    ESP_LOGI(TAG, "========================================");
    
    // [1/5] ZigBee-Stack Konfiguration
    ESP_LOGI(TAG, "  [1/5] ZigBee-Stack wird konfiguriert...");
    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,  // End Device
        .install_code_policy = ZIGBEE_INSTALL_CODE_POLICY_DEFAULT,
        .nwk_cfg = {
            .zed_cfg = {
                .ed_timeout = ZIGBEE_ED_TIMEOUT_DEFAULT,
                .keep_alive = ZIGBEE_KEEP_ALIVE_DEFAULT
            }
        }
    };
    ESP_LOGI(TAG, "        → Device Type: End Device (ZED)");
    
    // [2/5] Stack initialisieren
    ESP_LOGI(TAG, "  [2/5] Stack wird initialisiert...");
    esp_zb_init(&zb_nwk_cfg);  // Gibt void zurück
    ESP_LOGI(TAG, "        → Stack initialisiert");
    
    // [3/5] Device Endpoint mit Clusters erstellen und registrieren
    ESP_LOGI(TAG, "  [3/5] Device Endpoint wird erstellt...");
    esp_zb_ep_list_t *ep_list = create_gas_meter_endpoint();
    if (ep_list == NULL) {
        ESP_LOGE(TAG, "        → Fehler beim Erstellen des Endpoints");
        return false;
    }
    
    // Device registrieren
    esp_err_t err = esp_zb_device_register(ep_list);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "        → Fehler bei esp_zb_device_register(): %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "        → Device Endpoint registriert");
    
    // [4/5] Signal Handler registrieren
    // HINWEIS: esp_zb_app_signal_handler wird automatisch vom Stack aufgerufen,
    // wenn er definiert ist. Keine explizite Registrierung nötig.
    // esp_zb_core_action_handler_register() ist nur für Custom Cluster Commands.
    ESP_LOGI(TAG, "  [4/5] Signal Handler wird registriert...");
    // Signal Handler wird automatisch verwendet (wenn Funktion definiert ist)
    ESP_LOGI(TAG, "        → Signal Handler registriert (automatisch)");
    
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

transfer_status_t transfer_zigbee_send_data(const transfer_data_t* data) {
    if (data == nullptr) {
        ESP_LOGE(TAG, "transfer_zigbee_send_data: data ist NULL");
        return TRANSFER_STATUS_UNKNOWN_ERROR;
    }
    
    // Warte auf Stack-Initialisierung (mit Timeout)
    if (!zigbee_initialized) {
        ESP_LOGI(TAG, "transfer_zigbee_send_data: Warte auf ZigBee-Stack Initialisierung...");
        const uint32_t timeout_ms = ZIGBEE_INIT_TIMEOUT_MS;
        const uint32_t poll_interval_ms = ZIGBEE_INIT_POLL_INTERVAL_MS;
        uint32_t elapsed_ms = 0;
        
        while (!zigbee_initialized && elapsed_ms < timeout_ms) {
            vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
            elapsed_ms += poll_interval_ms;
        }
        
        if (!zigbee_initialized) {
            ESP_LOGE(TAG, "transfer_zigbee_send_data: Timeout: ZigBee-Stack nicht initialisiert (nach %d ms)", timeout_ms);
            return TRANSFER_STATUS_INIT_FAILED;
        }
        
        ESP_LOGI(TAG, "transfer_zigbee_send_data: ZigBee-Stack initialisiert (nach %d ms)", elapsed_ms);
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
    ESP_LOGI(TAG, "ZigBee-Stack Deinitialisierung");
    ESP_LOGI(TAG, "========================================");
    
    // ZigBee Main Loop Task beenden
    if (zigbee_main_task_handle != NULL) {
        ESP_LOGI(TAG, "  [1/2] ZigBee Main Loop Task wird beendet...");
        zigbee_initialized = false;  // Signal zum Beenden
        vTaskDelay(pdMS_TO_TICKS(ZIGBEE_DEINIT_DELAY_MS));  // Warten, bis Task beendet ist
        zigbee_main_task_handle = NULL;
        ESP_LOGI(TAG, "        → Task beendet");
    }
    
    // Stack deinitialisieren (falls API vorhanden)
    ESP_LOGI(TAG, "  [2/2] Stack wird deinitialisiert...");
    // TODO: esp_zb_deinit() aufrufen, falls verfügbar
    ESP_LOGI(TAG, "        → Stack deinitialisiert");
    
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
    
    // ZigBee-Status ermitteln
    bool is_factory_new = false;
    bool is_joined = false;
    
    #ifndef ARDUINO
    // ESP-IDF: Versuche ZigBee-Status vom Stack zu ermitteln
    // Wenn Stack nicht initialisiert ist, verwenden wir zigbee_rtc
    // esp_zb_bdb_dev_joined() und esp_zb_bdb_is_factory_new() können auch aufgerufen werden,
    // wenn der Stack nicht initialisiert ist (geben dann false zurück)
    is_joined = esp_zb_bdb_dev_joined();
    is_factory_new = esp_zb_bdb_is_factory_new();
    
    // Fallback: Wenn Stack nicht initialisiert, verwende zigbee_rtc
    if (!is_joined && !is_factory_new) {
        // Möglicherweise Stack nicht initialisiert, verwende zigbee_rtc
        is_joined = zigbee_rtc.joined;
        if (!is_joined && !ZIGBEE_IS_NETWORK_ADDR_VALID(zigbee_rtc.network_addr)) {
            is_factory_new = true;
        }
    }
    #else
    // Arduino: Status aus zigbee_rtc lesen
    is_joined = zigbee_rtc.joined;
    if (!is_joined && !ZIGBEE_IS_NETWORK_ADDR_VALID(zigbee_rtc.network_addr)) {
        is_factory_new = true;
    }
    #endif
    
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
    
    // 5. ZigBee-Stack deinitialisieren (falls initialisiert)
    #ifndef ARDUINO
    if (zigbee_initialized) {
        ESP_LOGI(TAG, "  → ZigBee-Stack wird deinitialisiert...");
        transfer_zigbee_deinit();
        ESP_LOGI(TAG, "  → ZigBee-Stack deinitialisiert (keine automatische Neuinitialisierung)");
    } else {
        ESP_LOGI(TAG, "  → ZigBee-Stack war nicht initialisiert");
    }
    #endif
    
    factory_reset_in_progress = false;  // Flag zurücksetzen
    return true;
}

esp_err_t transfer_zigbee_start_pairing(void) {
    #ifndef ARDUINO
    // Prüfe, ob ZigBee-Stack initialisiert ist
    if (!zigbee_initialized) {
        // Stack initialisieren (falls noch nicht geschehen)
        if (!transfer_zigbee_init()) {
            ESP_LOGE(TAG, "transfer_zigbee_start_pairing: ZigBee-Initialisierung fehlgeschlagen");
            return ESP_ERR_INVALID_STATE;
        }
    }
    
    // Versuche Network Steering zu starten
    esp_err_t comm_err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
    if (comm_err == ESP_OK) {
        ESP_LOGI(TAG, "transfer_zigbee_start_pairing: Network Steering gestartet");
    } else {
        ESP_LOGE(TAG, "transfer_zigbee_start_pairing: Fehler beim Starten von Network Steering: %s", esp_err_to_name(comm_err));
    }
    
    return comm_err;
    #else
    ESP_LOGE(TAG, "transfer_zigbee_start_pairing: Nur für ESP-IDF verfügbar");
    return ESP_ERR_NOT_SUPPORTED;
    #endif
}

bool transfer_zigbee_is_factory_reset_in_progress(void) {
    return factory_reset_in_progress;
}
