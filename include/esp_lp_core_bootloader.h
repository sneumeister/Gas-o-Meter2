/*
 * ESP-IDF LP-Core Bootloader Header
 * Diese Header-Datei ist eine vereinfachte Version für Arduino Framework
 * Original: components/ulp/lp_core/include/esp_lp_core_bootloader.h
 */

#ifndef ESP_LP_CORE_BOOTLOADER_H
#define ESP_LP_CORE_BOOTLOADER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LP-Core bootloader configuration
 */
typedef struct {
    uint32_t entry_addr;  // Entry address for LP-Core main function
} esp_lp_core_bootloader_config_t;

/**
 * @brief Initialize LP-Core bootloader
 * @param config Bootloader configuration
 * @return ESP_OK on success
 */
esp_err_t esp_lp_core_bootloader_init(const esp_lp_core_bootloader_config_t *config);

/**
 * @brief Deinitialize LP-Core bootloader
 * @return ESP_OK on success
 */
esp_err_t esp_lp_core_bootloader_deinit(void);

// Stub-Implementierungen (werden in esp_lp_core_stub.cpp definiert)
// Diese Funktionen sind Platzhalter, bis die echten ESP-IDF Funktionen verfügbar sind

#ifdef __cplusplus
}
#endif

#endif // ESP_LP_CORE_BOOTLOADER_H

