#include "wifi_manager.h"
#include "hardware.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include <cstring>

static const char* TAG = "wifi_manager";

#define WIFI_BIT_GOT_IP (1u << 0)
#define WIFI_STA_CONNECT_TIMEOUT_MS 20000

static EventGroupHandle_t s_wifi_events = nullptr;
static esp_event_handler_instance_t s_instance_wifi = nullptr;
static esp_event_handler_instance_t s_instance_ip = nullptr;

static bool s_wifi_initialized = false;
static bool s_wifi_connected = false;
static wifi_ap_record_t s_ap_info = {};
static esp_netif_ip_info_t s_wifi_ip_info = {};

/** Connect-Pfad: große Strukturen nicht auf Main-Task-Stack (MQTT-Timer-Wake). */
static wifi_manager_sta_config_t s_connect_cfg;
struct wifi_connect_candidate_t {
    const char* ssid;
    const char* password;
    int rssi;
    wifi_ap_record_t ap_rec;
};
static wifi_connect_candidate_t s_connect_candidates[2];
static wifi_ap_record_t s_connect_scan_rec;

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    (void)arg;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi Station gestartet");
                break;
            case WIFI_EVENT_STA_CONNECTED: {
                auto* event = static_cast<wifi_event_sta_connected_t*>(event_data);
                ESP_LOGI(TAG, "Verbunden mit SSID: %s", event->ssid);
                break;
            }
            case WIFI_EVENT_STA_DISCONNECTED:
                s_wifi_connected = false;
                if (s_wifi_events != nullptr) {
                    xEventGroupClearBits(s_wifi_events, WIFI_BIT_GOT_IP);
                }
                if (event_data != nullptr) {
                    auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
                    ESP_LOGI(TAG, "WiFi getrennt (reason=%d)", (int)event->reason);
                } else {
                    ESP_LOGI(TAG, "WiFi getrennt");
                }
                break;
            case WIFI_EVENT_AP_STACONNECTED: {
                auto* event = static_cast<wifi_event_ap_staconnected_t*>(event_data);
                ESP_LOGI(TAG, "AP Client verbunden: %02X:%02X:%02X:%02X:%02X:%02X, AID=%d",
                         event->mac[0], event->mac[1], event->mac[2], event->mac[3], event->mac[4],
                         event->mac[5], event->aid);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                auto* event = static_cast<wifi_event_ap_stadisconnected_t*>(event_data);
                ESP_LOGI(TAG, "AP Client getrennt: %02X:%02X:%02X:%02X:%02X:%02X, AID=%d",
                         event->mac[0], event->mac[1], event->mac[2], event->mac[3], event->mac[4],
                         event->mac[5], event->aid);
                break;
            }
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        s_wifi_ip_info = event->ip_info;
        s_wifi_connected = true;
        if (s_wifi_events != nullptr) {
            xEventGroupSetBits(s_wifi_events, WIFI_BIT_GOT_IP);
        }
        ESP_LOGI(TAG, "IP erhalten: " IPSTR, IP2STR(&s_wifi_ip_info.ip));
    }
}

bool wifi_manager_set_tx_power_quarter_dbm(int8_t tx_power_quarter) {
    esp_err_t ret = esp_wifi_set_max_tx_power(tx_power_quarter);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi TX Power gesetzt: %d (%.2f dBm)", tx_power_quarter, tx_power_quarter * 0.25f);
        return true;
    }
    ESP_LOGW(TAG, "WiFi TX Power konnte nicht gesetzt werden: %s (Wert: %d)", esp_err_to_name(ret),
             tx_power_quarter);
    return false;
}

bool wifi_manager_init(void) {
    if (s_wifi_initialized) {
        return true;
    }

    if (s_wifi_events == nullptr) {
        s_wifi_events = xEventGroupCreate();
        if (s_wifi_events == nullptr) {
            ESP_LOGE(TAG, "EventGroup konnte nicht erstellt werden");
            return false;
        }
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init fehlgeschlagen: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default fehlgeschlagen: %s", esp_err_to_name(err));
        return false;
    }
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi-Initialisierung fehlgeschlagen: %s", esp_err_to_name(ret));
        return false;
    }

    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    if (s_instance_wifi == nullptr) {
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr,
                                            &s_instance_wifi);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr,
                                            &s_instance_ip);
    }

    s_wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi initialisiert");
    return true;
}

bool wifi_manager_is_initialized(void) {
    return s_wifi_initialized;
}

bool wifi_manager_session_begin(void) {
    if (!wifi_manager_load_sta_config(&s_connect_cfg)) {
        return false;
    }
    if (!wifi_manager_init()) {
        return false;
    }

    wifi_manager_platform_stop_dns_captive();
    esp_wifi_set_mode(WIFI_MODE_STA);

    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi-Start fehlgeschlagen: %s", esp_err_to_name(ret));
        return false;
    }

    wifi_manager_set_tx_power_quarter_dbm((int8_t)(s_connect_cfg.wifi_tx_power_dbm * 4));
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif != nullptr) {
        esp_netif_set_hostname(sta_netif, s_connect_cfg.hostname);
        ESP_LOGI(TAG, "Hostname gesetzt: %s", s_connect_cfg.hostname);
    }
    return true;
}

bool wifi_connect_sta(void) {
    if (!wifi_manager_load_sta_config(&s_connect_cfg)) {
        ESP_LOGE(TAG, "Keine WiFi-Credentials verfügbar");
        return false;
    }
    if (s_connect_cfg.wifi_count == 0) {
        ESP_LOGE(TAG, "Keine WiFi-Credentials verfügbar");
        return false;
    }

    if (!wifi_manager_session_begin()) {
        return false;
    }

    ESP_LOGI(TAG, "WiFi-Credentials konfiguriert: %u", (unsigned)s_connect_cfg.wifi_count);
    for (uint8_t i = 0; i < s_connect_cfg.wifi_count && i < 2; i++) {
        ESP_LOGI(TAG, "  Kandidat[%u]: SSID=%s", (unsigned)i, s_connect_cfg.ssid[i]);
    }

    wifi_scan_config_t scan_config = {};
    uint16_t ap_count = 0;
    const int scan_retries = 3;
    esp_err_t ret = ESP_OK;
    for (int scan_try = 1; scan_try <= scan_retries; scan_try++) {
        ret = esp_wifi_scan_start(&scan_config, true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "WiFi-Scan fehlgeschlagen (Versuch %d/%d): %s", scan_try, scan_retries,
                     esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        esp_wifi_scan_get_ap_num(&ap_count);
        ESP_LOGI(TAG, "WiFi-Scan Versuch %d/%d: %u Netzwerke gefunden", scan_try, scan_retries,
                 (unsigned)ap_count);
        if (ap_count > 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (ap_count == 0) {
        ESP_LOGE(TAG, "Keine Netzwerke gefunden");
        return false;
    }

    uint8_t candidate_count = 0;

    while (candidate_count < 2 && esp_wifi_scan_get_ap_record(&s_connect_scan_rec) == ESP_OK) {
        for (uint8_t i = 0; i < s_connect_cfg.wifi_count && i < 2; i++) {
            if (strcmp((const char*)s_connect_scan_rec.ssid, s_connect_cfg.ssid[i]) != 0) {
                continue;
            }
            bool already = false;
            for (uint8_t c = 0; c < candidate_count; c++) {
                if (strcmp(s_connect_candidates[c].ssid, s_connect_cfg.ssid[i]) == 0) {
                    already = true;
                    break;
                }
            }
            if (already) {
                break;
            }
            s_connect_candidates[candidate_count].ssid = s_connect_cfg.ssid[i];
            s_connect_candidates[candidate_count].password = s_connect_cfg.password[i];
            s_connect_candidates[candidate_count].rssi = s_connect_scan_rec.rssi;
            s_connect_candidates[candidate_count].ap_rec = s_connect_scan_rec;
            ESP_LOGI(TAG, "Bekannte SSID gefunden: %s (RSSI: %d dBm)",
                     s_connect_candidates[candidate_count].ssid, s_connect_scan_rec.rssi);
            candidate_count++;
            break;
        }
    }
    esp_wifi_clear_ap_list();

    if (candidate_count == 0) {
        ESP_LOGE(TAG, "Kein bekanntes Netzwerk gefunden");
        return false;
    }

    for (uint8_t c = 0; c < candidate_count; c++) {
        const char* selected_ssid = s_connect_candidates[c].ssid;
        const char* selected_password = s_connect_candidates[c].password;

        ESP_LOGI(TAG, "Verbinde mit: %s (RSSI: %d dBm, Versuch %u/%u)", selected_ssid,
                 s_connect_candidates[c].rssi, (unsigned)(c + 1), (unsigned)candidate_count);

        wifi_config_t wifi_config = {};
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        strncpy((char*)wifi_config.sta.ssid, selected_ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char*)wifi_config.sta.password, selected_password, sizeof(wifi_config.sta.password) - 1);

        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        s_wifi_connected = false;
        if (s_wifi_events != nullptr) {
            xEventGroupClearBits(s_wifi_events, WIFI_BIT_GOT_IP);
        }

        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_wifi_connect();

        EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_BIT_GOT_IP, pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(WIFI_STA_CONNECT_TIMEOUT_MS));

        if ((bits & WIFI_BIT_GOT_IP) != 0 && s_wifi_connected) {
            s_ap_info = s_connect_candidates[c].ap_rec;
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&s_wifi_ip_info.ip));
            ESP_LOGI(TAG, "WiFi verbunden! IP: %s", ip_str);
            return true;
        }

        ESP_LOGW(TAG, "Verbindung zu %s fehlgeschlagen, versuche nächstes bekanntes Netz...", selected_ssid);
    }

    ESP_LOGE(TAG, "WiFi-Verbindung fehlgeschlagen");
    return false;
}

void wifi_manager_session_end(void) {
    if (!s_wifi_initialized) {
        return;
    }
    ESP_LOGI(TAG, "Trenne WiFi...");
    if (s_wifi_connected) {
        esp_err_t ret = esp_wifi_disconnect();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_disconnect() gab Fehler zurück: %s", esp_err_to_name(ret));
        }
        s_wifi_connected = false;
        if (s_wifi_events != nullptr) {
            xEventGroupClearBits(s_wifi_events, WIFI_BIT_GOT_IP);
        }
    }
    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop() gab Fehler zurück: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "WiFi getrennt und gestoppt");
}

bool wifi_start_access_point(void) {
    wifi_manager_sta_config_t cfg = {};
    if (!wifi_manager_load_sta_config(&cfg)) {
        return false;
    }
    if (!wifi_manager_init()) {
        return false;
    }

    ESP_LOGI(TAG, "Starte Access Point...");

    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_wifi_set_mode(WIFI_MODE_APSTA);

    size_t ssid_len = strlen(cfg.hostname);
    if (ssid_len > 31) {
        ssid_len = 31;
        ESP_LOGW(TAG, "SSID zu lang, gekürzt auf: %.*s", (int)ssid_len, cfg.hostname);
    }

    wifi_config_t ap_config = {};
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.pmf_cfg.capable = true;
    ap_config.ap.pmf_cfg.required = false;
    strncpy((char*)ap_config.ap.ssid, cfg.hostname, ssid_len);
    ap_config.ap.ssid[ssid_len] = '\0';
    ap_config.ap.ssid_len = (uint8_t)ssid_len;
    ap_config.ap.password[0] = '\0';

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AP-Konfiguration fehlgeschlagen: %s", esp_err_to_name(ret));
        return false;
    }

    wifi_config_t sta_clear = {};
    sta_clear.sta.ssid[0] = '\0';
    sta_clear.sta.password[0] = '\0';
    sta_clear.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ret = esp_wifi_set_config(WIFI_IF_STA, &sta_clear);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "STA-Config (leer) fehlgeschlagen: %s", esp_err_to_name(ret));
        return false;
    }

    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif != nullptr) {
        esp_netif_ip_info_t ip_info;
        IP4_ADDR(&ip_info.ip, AP_IP_ADDRESS_1, AP_IP_ADDRESS_2, AP_IP_ADDRESS_3, AP_IP_ADDRESS_4);
        IP4_ADDR(&ip_info.gw, AP_IP_ADDRESS_1, AP_IP_ADDRESS_2, AP_IP_ADDRESS_3, AP_IP_ADDRESS_4);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        esp_netif_dhcps_stop(ap_netif);
        esp_netif_set_ip_info(ap_netif, &ip_info);
        esp_netif_dhcps_start(ap_netif);
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AP-Start fehlgeschlagen: %s", esp_err_to_name(ret));
        return false;
    }

    wifi_manager_set_tx_power_quarter_dbm((int8_t)(cfg.wifi_tx_power_dbm * 4));

    ESP_LOGI(TAG, "Access Point gestartet (Modus AP+STA, Captive nutzt nur AP): %s", cfg.hostname);
    ESP_LOGI(TAG, "AP IP: %d.%d.%d.%d", AP_IP_ADDRESS_1, AP_IP_ADDRESS_2, AP_IP_ADDRESS_3, AP_IP_ADDRESS_4);
    ESP_LOGI(TAG, "WLAN ist offen (kein Passwort)");

    if (!wifi_manager_platform_start_dns_captive()) {
        ESP_LOGW(TAG, "DNS Captive konnte nicht gestartet werden (HTTP-Captive-Redirect bleibt aktiv)");
    }
    return true;
}

bool wifi_is_connected(void) {
    return s_wifi_connected;
}

const wifi_ap_record_t* wifi_get_ap_info(void) {
    return &s_ap_info;
}

const esp_netif_ip_info_t* wifi_get_ip_info(void) {
    return &s_wifi_ip_info;
}

bool wifi_manager_apply_tx_power(void) {
    wifi_manager_sta_config_t cfg = {};
    if (!wifi_manager_load_sta_config(&cfg)) {
        return false;
    }
    return wifi_manager_set_tx_power_quarter_dbm((int8_t)(cfg.wifi_tx_power_dbm * 4));
}
