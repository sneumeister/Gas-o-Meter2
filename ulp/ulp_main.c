// LP-Core Programm für ESP32C6
// Läuft auf dem separaten RISC-V LP-Core-Prozessor

#include "ulp_lp_core.h"
#include "ulp_lp_core_utils.h"

// Globale Variablen im LP-RAM (werden von ulp_embed_binary() mit ulp_ Präfix exportiert)
// WICHTIG: volatile — HP-Core und LP-Core greifen gleichzeitig zu
volatile uint32_t lp_core_running = 0;  // Watchdog-Lebenszeichen (regelmäßig erhöhen!)
volatile uint32_t pulse_counter = 0;    // Puls-Zähler (nur bei HIGH→LOW-Flanke)

int main(void)
{
    const uint32_t wakeup_cause = ulp_lp_core_get_wakeup_cause();

    // Jeder LP-Start bestätigt, dass Programm und Wake-Quellen funktionieren.
    lp_core_running++;

    // GPIO2 ist der einzige als LP_IO-Wake konfigurierte Pin.
    if (wakeup_cause & ULP_LP_CORE_WAKEUP_SOURCE_LP_IO) {
        pulse_counter++;
    }

    // Die IDF-Startup-Routine programmiert anschließend den nächsten
    // LP-Timer-Termin und versetzt den LP-Core in den Haltzustand.
    return 0;
}
