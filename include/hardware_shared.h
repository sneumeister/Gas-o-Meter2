#ifndef HARDWARE_SHARED_H
#define HARDWARE_SHARED_H

// Gemeinsame Hardware-Konstanten für HP-Core und LP-Core.
// Keine FreeRTOS-/Driver-Includes — LP-Übersetzungseinheit darf diese Datei einbinden.
//
// Pinmap gilt für Seeed Studio XIAO ESP32-C6
// (platformio.ini: board = seeed_xiao_esp32c6). Silkscreen D0…D10 ≠ GPIO-Nummer
// auf anderen Modulen — bei neuem Board eigenen Block + Build-Flag ergänzen.

// ============================================
// Hardware Pin-Definitionen (Seeed XIAO ESP32-C6)
// ============================================

#define BUTTON_A_GPIO      1   // XIAO D1 — Taster A (Software Pull-Up, active-low)
#define REED_GPIO          2   // XIAO D2 — Reed-Kontakt (externer Pull-Up, active-low)
#define BUTTON_B_GPIO      21  // XIAO D3 — Taster B (Software Pull-Up, active-low)

// ============================================
// LP-Core Abtastintervalle
// ============================================
// TPL5110 mit R1 = 8,2 kΩ: LOW mind. ~3 s, Mindestabstand ~4 s.
#define LP_CORE_SAMPLE_IDLE_US   2000000ULL  // GPIO2 HIGH: nächste Probe in 2 s
#define LP_CORE_SAMPLE_PULSE_US   250000ULL  // GPIO2 LOW: Rückkehr auf HIGH schnell sehen

#endif // HARDWARE_SHARED_H
