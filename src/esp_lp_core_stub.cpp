/*
 * ESP-IDF LP-Core Stub Implementation
 * Diese Implementierung ist ein Platzhalter für die echten ESP-IDF Funktionen.
 * Sobald die LP-Core API im Arduino Framework verfügbar ist, können diese
 * Stubs durch die echten Funktionen ersetzt werden.
 */

#include "esp_lp_core.h"
#include "esp_lp_core_bootloader.h"
#include "esp_err.h"
#include <Arduino.h>

static bool lp_core_running_flag = false;

bool esp_lp_core_is_running(void) {
    // Stub: Gibt immer false zurück, da LP-Core im Arduino Framework noch nicht verfügbar ist
    Serial.println("[LP-Core Stub] esp_lp_core_is_running() - Stub-Implementierung (LP-Core nicht verfügbar)");
    return lp_core_running_flag;
}

esp_err_t esp_lp_core_start(void) {
    // Stub: Gibt ESP_ERR_NOT_SUPPORTED zurück
    Serial.println("[LP-Core Stub] esp_lp_core_start() - Stub-Implementierung (LP-Core nicht verfügbar)");
    lp_core_running_flag = false;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_lp_core_stop(void) {
    // Stub: Gibt ESP_ERR_NOT_SUPPORTED zurück
    Serial.println("[LP-Core Stub] esp_lp_core_stop() - Stub-Implementierung (LP-Core nicht verfügbar)");
    lp_core_running_flag = false;
    return ESP_ERR_NOT_SUPPORTED;
}

void esp_lp_core_delay(uint32_t us) {
    // Stub: Leere Implementierung (wird im LP-Core Code verwendet)
    // Diese Funktion sollte nur im LP-Core Kontext aufgerufen werden
    (void)us;
}

esp_err_t esp_lp_core_bootloader_init(const esp_lp_core_bootloader_config_t *config) {
    // Stub: Gibt ESP_ERR_NOT_SUPPORTED zurück
    Serial.println("[LP-Core Stub] esp_lp_core_bootloader_init() - Stub-Implementierung (LP-Core nicht verfügbar)");
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_lp_core_bootloader_deinit(void) {
    // Stub: Gibt ESP_ERR_NOT_SUPPORTED zurück
    Serial.println("[LP-Core Stub] esp_lp_core_bootloader_deinit() - Stub-Implementierung (LP-Core nicht verfügbar)");
    return ESP_ERR_NOT_SUPPORTED;
}

