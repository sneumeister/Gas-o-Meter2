#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif.h"
#include "esp_wifi.h"
#include "hardware.h"

#ifdef __cplusplus
extern "C" {
#endif

/** STA-Credentials und Parameter aus der Geräte-Config (main). */
typedef struct {
    uint8_t wifi_count;
    char ssid[2][32];
    char password[2][64];
    int8_t wifi_tx_power_dbm;
    char hostname[HOSTNAME_MAX_LEN + 1];
} wifi_manager_sta_config_t;

/** Lädt Config aus RTC (Implementierung in main_idf.cpp). */
bool wifi_manager_load_sta_config(wifi_manager_sta_config_t* out);

/** DNS-Captive-Hooks für AP/STA-Wechsel (Implementierung in main_idf.cpp). */
void wifi_manager_platform_stop_dns_captive(void);
bool wifi_manager_platform_start_dns_captive(void);

/** Einmalig: Netif, Event-Loop, WiFi-Treiber, Event-Handler. */
bool wifi_manager_init(void);

bool wifi_manager_is_initialized(void);

/** STA-Session: Captive-DNS aus, Modus STA, ein esp_wifi_start, TX-Power. */
bool wifi_manager_session_begin(void);

/** Scan (sync), Verbindung mit EventGroup (ein Start pro Aufruf). */
bool wifi_connect_sta(void);

/** disconnect + esp_wifi_stop. */
void wifi_manager_session_end(void);

/** AP+STA-Konfigurations-AP (Ersteinrichtung). */
bool wifi_start_access_point(void);

bool wifi_is_connected(void);

const wifi_ap_record_t* wifi_get_ap_info(void);

const esp_netif_ip_info_t* wifi_get_ip_info(void);

/** TX Power in 0,25-dBm-Einheiten (nach esp_wifi_start). */
bool wifi_manager_set_tx_power_quarter_dbm(int8_t tx_power_quarter);

/** Setzt TX Power aus wifi_manager_load_sta_config(). */
bool wifi_manager_apply_tx_power(void);

#ifdef __cplusplus
}
#endif

#endif
