#pragma once

/**
 * Max. AP-Einträge für die Web-UI (/wifi/scan). Verarbeitung per
 * esp_wifi_scan_get_ap_record() (ein Eintrag pro Aufruf); Rest mit
 * esp_wifi_clear_ap_list() freigeben.
 *
 * wifi_connect_sta() nutzt dieselbe API sequentiell und braucht keinen großen Puffer.
 * Die Scan-Liste ist nach RSSI absteigend sortiert (stärkste zuerst).
 */
#define WIFI_SCAN_MAX_AP 10

/** IEEE 802.11: SSID max. 32 Oktette (ohne terminierendes NUL in der Luft). */
#define WIFI_SCAN_SSID_MAX_LEN 32
/** Puffergröße inkl. '\0' für C-Strings. */
#define WIFI_SCAN_SSID_BUF_LEN (WIFI_SCAN_SSID_MAX_LEN + 1)
