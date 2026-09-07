// LP-Core Programm für ESP32C6
// Läuft auf dem separaten RISC-V LP-Core-Prozessor
// Haltbetrieb: LP-Timer weckt, Pegelvergleich erkennt HIGH→LOW (kein LP_IO-Wake).

#include "hardware_shared.h"
#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_lp_timer_shared.h"
#include "ulp_lp_core_memory_shared.h"
#include "ulp_lp_core_utils.h"

// Globale Variablen im LP-RAM (werden von ulp_embed_binary() mit ulp_ Präfix exportiert)
// WICHTIG: volatile — HP-Core und LP-Core greifen gleichzeitig zu
volatile uint32_t lp_core_running = 0;  // Watchdog-Lebenszeichen
volatile uint32_t pulse_counter = 0;    // Gasimpulse (nur HIGH->LOW)
volatile uint32_t reed_level = 0;       // letzter Pegel; HP-Core setzt den Startwert

int main(void)
{
    const lp_io_num_t reed_io = (lp_io_num_t)REED_GPIO;

    ulp_lp_core_gpio_init(reed_io);
    ulp_lp_core_gpio_input_enable(reed_io);

    lp_core_running++;

    const uint32_t level = ulp_lp_core_gpio_get_level(reed_io);
    if (level == 0 && reed_level != 0) {
        pulse_counter++;
    }
    reed_level = level;

    ulp_lp_core_memory_shared_cfg_get()->sleep_duration_ticks =
        ulp_lp_core_lp_timer_calculate_sleep_ticks(level ? LP_CORE_SAMPLE_IDLE_US
                                                        : LP_CORE_SAMPLE_PULSE_US);

    return 0;
}
