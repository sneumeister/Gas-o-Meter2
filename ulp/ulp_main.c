// LP-Core Programm für ESP32C6
// Läuft auf dem separaten RISC-V LP-Core-Prozessor

#include "ulp_lp_core.h"
#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_utils.h"

// Globale Variablen im RTC-RAM (werden von ulp_embed_binary() mit ulp_ Präfix exportiert)
// WICHTIG: volatile — HP-Core und LP-Core greifen gleichzeitig zu
volatile uint32_t lp_core_running = 0;  // Watchdog-Lebenszeichen (regelmäßig erhöhen!)
volatile uint32_t pulse_counter = 0;    // Puls-Zähler (nur bei HIGH→LOW-Flanke)

#define REED_LP_IO_NUM LP_IO_NUM_2

#define LP_CORE_INTERVAL_US  (2500000ULL)  // 2,5 s zwischen Polls im HIGH-Idle
#define HEARTBEAT_CHUNK_US   (100000U)     // 100 ms — Watchdog auf HP-Core (~6 s Timeout)
#define POLL_INTERVAL_US     (10000U)      // 10 ms während LOW-Warte

static void lp_core_heartbeat_delay_us(uint32_t total_us)
{
    while (total_us >= HEARTBEAT_CHUNK_US) {
        lp_core_running++;
        ulp_lp_core_delay_us(HEARTBEAT_CHUNK_US);
        total_us -= HEARTBEAT_CHUNK_US;
    }
    if (total_us > 0) {
        lp_core_running++;
        ulp_lp_core_delay_us(total_us);
    }
}

int main(void) {
    ulp_lp_core_gpio_init(REED_LP_IO_NUM);
    ulp_lp_core_gpio_input_enable(REED_LP_IO_NUM);
    lp_core_running++;

    // Idle = HIGH. prev_level emuliert Flankenerkennung (kein GPIO-Interrupt auf LP-Core):
    // Nur der Wechsel HIGH→LOW zählt; verhindert Doppelzählung bei Dauer-LOW / LP-Neustart.
    uint32_t prev_level = ulp_lp_core_gpio_get_level(REED_LP_IO_NUM);

    while (1) {
        uint32_t reed_level = ulp_lp_core_gpio_get_level(REED_LP_IO_NUM);

        // Emulierte Fallende Flanke (HIGH→LOW), analog zu FALLING-Interrupt
        if (reed_level == 0 && prev_level != 0) {
            pulse_counter++;
        }

        if (reed_level == 0) {
            while (ulp_lp_core_gpio_get_level(REED_LP_IO_NUM) == 0) {
                lp_core_running++;
                ulp_lp_core_delay_us(POLL_INTERVAL_US);
            }
            prev_level = 1;
        } else {
            prev_level = reed_level;
        }

        lp_core_running++;
        lp_core_heartbeat_delay_us(LP_CORE_INTERVAL_US);
    }

    return 0;
}
