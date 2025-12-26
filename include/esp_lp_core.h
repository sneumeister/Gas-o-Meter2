/*
 * ESP-IDF LP-Core Header
 * Diese Header-Datei ist eine vereinfachte Version für Arduino Framework
 * Original: components/ulp/lp_core/include/esp_lp_core.h
 */

#ifndef ESP_LP_CORE_H
#define ESP_LP_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check if LP-Core is running
 * @return true if LP-Core is running, false otherwise
 */
bool esp_lp_core_is_running(void);

/**
 * @brief Start the LP-Core
 * @return ESP_OK on success
 */
esp_err_t esp_lp_core_start(void);

/**
 * @brief Stop the LP-Core
 * @return ESP_OK on success
 */
esp_err_t esp_lp_core_stop(void);

/**
 * @brief Delay function for LP-Core (in microseconds)
 * @param us Delay in microseconds
 */
void esp_lp_core_delay(uint32_t us);

#ifdef __cplusplus
}
#endif

#endif // ESP_LP_CORE_H

