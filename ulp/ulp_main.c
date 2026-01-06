// LP-Core Programm für ESP32C6
// Läuft auf dem separaten RISC-V LP-Core-Prozessor

#include "ulp_lp_core.h"
#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_utils.h"

// Globale Variablen im RTC-RAM (werden von ulp_embed_binary() mit ulp_ Präfix exportiert)
// Diese Variablen werden im generierten Header lp_core.h als ulp_lp_core_running und ulp_pulse_counter exportiert
// WICHTIG: Variablen müssen hier DEFINIERT werden (nicht extern), damit ulp_embed_binary() sie exportiert!
// WICHTIG: volatile ist erforderlich, da beide Prozessoren (HP-Core und LP-Core) gleichzeitig darauf zugreifen!
volatile uint32_t lp_core_running = 0;  // Watchdog-Zähler (wird regelmäßig erhöht)
volatile uint32_t pulse_counter = 0;    // Puls-Zähler (wird bei REED==LOW sofort erhöht)

// GPIO-Nummer für REED-Pin (muss als LP_IO_NUM definiert sein)
// GPIO2 auf ESP32C6 entspricht LP_IO_NUM_2
#define REED_LP_IO_NUM LP_IO_NUM_2

// LP-Core Intervall: 2.5s (aus hardware.h: REED_MIN_PULSE_DURATION_US - 0.5s)
// TPL5110 zeigt Signallängen von 3-4.5s, daher ist 2.5s Intervall ausreichend
#define LP_CORE_INTERVAL_US  (2500000ULL)  // 2.5s = 2500000µs

int main(void) {
    // REED-Pin initialisieren (Input, kein Pull-Up/Pull-Down - externer Pull-Up)
    ulp_lp_core_gpio_init(REED_LP_IO_NUM);
    ulp_lp_core_gpio_input_enable(REED_LP_IO_NUM);
    lp_core_running++;

    while (1) {
        // Prüfe REED-Pin Status
        uint32_t reed_level = ulp_lp_core_gpio_get_level(REED_LP_IO_NUM);
        
        if (reed_level == 0) {  // LOW (active-low) → Puls erkannt
            // REED ist LOW → Puls sofort zählen
            // TPL5110 sorgt für saubere Signale (kein Debouncing nötig)
            pulse_counter++;
            
            // Watchdog-Zähler erhöhen (für HP-Core-Monitoring)
            lp_core_running++;
            
            // Warten bis REED HIGH wird (Puls-Ende)
            // Verhindert Doppelzählung bei "immer noch LOW" im nächsten Intervall
            // TPL5110 zeigt Schwankungen von 3-4.5s, daher warten bis HIGH
            // Polling mit kurzen Delays (10ms) für minimale Energieaufnahme
            const uint32_t POLL_INTERVAL_US = 10000;  // 10ms = 10000µs
            
            while (ulp_lp_core_gpio_get_level(REED_LP_IO_NUM) == 0) {
                // Kurze Verzögerung (10ms) - energieeffizientes Polling
                ulp_lp_core_delay_us(POLL_INTERVAL_US);
            }
            
            // REED ist jetzt HIGH → Puls-Ende
            // Watchdog-Zähler erneut erhöhen
        }
        lp_core_running++;
        // Warten für LP_CORE_INTERVAL_US
        // LP_CORE_INTERVAL_US = 2.5s = 2500000µs
        // Verwendet LP-Core Delay-Funktion (busy-wait, aber LP-Core läuft weiter)
        ulp_lp_core_delay_us(LP_CORE_INTERVAL_US);
    }
    
    return 0;
}
