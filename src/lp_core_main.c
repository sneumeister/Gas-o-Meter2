// ESP-IDF LP-Core Header
// HINWEIS: Mit framework = arduino, espidf sind die ESP-IDF Header verfügbar
#include <esp_lp_core.h>
#include <driver/gpio.h>
#include "hardware.h"

// RTC-RAM Variablen (geteilt mit HP-Core)
extern uint32_t pulse_counter;
extern uint32_t lp_core_running;

// LP-Core Hauptfunktion
// Diese Funktion wird vom HP-Core gestartet, sobald die LP-Core API verfügbar ist.
void lp_core_main(void) {
    // REED_GPIO konfigurieren (als Eingang)
    gpio_set_direction(REED_GPIO, GPIO_MODE_INPUT);
    
    // Dauerschleife
    while (1) {
        // Watchdog-Zähler erhöhen (jeder Schleifendurchlauf)
        lp_core_running++;
        
        // Prüfe REED_GPIO
        if (gpio_get_level(REED_GPIO) == LOW) {  // LOW erkannt (Pulse)
            // Pulse erkannt → pulse_counter erhöhen
            pulse_counter++;
            
            // Warte bis Signal wieder HIGH ist (alle 100ms prüfen)
            while (gpio_get_level(REED_GPIO) == LOW) {
                // 100ms warten
                esp_lp_core_delay(100000);  // 100ms in Mikrosekunden
            }
        }
        
        // Nach jedem Schleifendurchlauf: LP_CORE_INTERVAL warten
        // (damit kein Puls verpasst wird, da Pulse 3-4.5s dauern)
        esp_lp_core_delay(LP_CORE_INTERVAL_US);
    }
}

