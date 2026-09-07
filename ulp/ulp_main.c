// LP-Core Programm für ESP32C6
// Läuft auf dem separaten RISC-V LP-Core-Prozessor
// Haltbetrieb: LP-Timer weckt, Pegelvergleich erkennt HIGH→LOW (kein LP_IO-Wake).

#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_lp_timer_shared.h"
#include "ulp_lp_core_memory_shared.h"
#include "ulp_lp_core_utils.h"

#define REED_LP_IO_NUM     LP_IO_NUM_2

// Muss zu LP_CORE_SAMPLE_* in include/hardware.h passen.
#define SAMPLE_IDLE_US     2000000UL   // GPIO2 HIGH: naechste Probe in 2 s
#define SAMPLE_PULSE_US     250000UL   // GPIO2 LOW: Rueckkehr auf HIGH schnell sehen

// Globale Variablen im LP-RAM (werden von ulp_embed_binary() mit ulp_ Präfix exportiert)
// WICHTIG: volatile — HP-Core und LP-Core greifen gleichzeitig zu
volatile uint32_t lp_core_running = 0;  // Watchdog-Lebenszeichen
volatile uint32_t pulse_counter = 0;    // Gasimpulse (nur HIGH->LOW)
volatile uint32_t reed_level = 0;       // letzter Pegel; HP-Core setzt den Startwert

int main(void)
{
    ulp_lp_core_gpio_init(REED_LP_IO_NUM);
    ulp_lp_core_gpio_input_enable(REED_LP_IO_NUM);

    lp_core_running++;

    const uint32_t level = ulp_lp_core_gpio_get_level(REED_LP_IO_NUM);
    if (level == 0 && reed_level != 0) {
        pulse_counter++;
    }
    reed_level = level;

    ulp_lp_core_memory_shared_cfg_get()->sleep_duration_ticks =
        ulp_lp_core_lp_timer_calculate_sleep_ticks(level ? SAMPLE_IDLE_US
                                                        : SAMPLE_PULSE_US);

    return 0;
}
