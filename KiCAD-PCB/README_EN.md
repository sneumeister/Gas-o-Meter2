<!-- translation-source: README.md -->
<!-- translation-source-blob: 1f17920d5937ae5a817f15734942bb4aa68d30b6 -->

[↓ Wechsel zu Deutsch](README.md)

# Gas-O-Meter2 — KiCAD Boards

KiCAD schematics, layouts, and BOMs for Gas-O-Meter2 hardware (ESP32-C6).

## Operating principle

Gas meter ticks are captured with a **reed switch**. The ESP32-C6 has no suitable **Schmitt-trigger input** for clean pulse detection — the **TPL5110** microtimer compensates: it debounces the reed switch and, via the **8.2 kΩ resistor**, produces a defined pulse of about **3.5 s** on each tick.

## Reed orientation in the housing

Align the reed switch in the housing cutout as shown — so the magnet in the gas meter closes it reliably:

![Funktionierende Reed-Ausrichtung](../pictures/reedausrichtung.png)

## Board revisions

### [Version 20251022](Version20251022/README_EN.md) — first revision

- Battery voltage on **GPIO0** (Pin1/A0/D0) via **1:1 voltage divider** (R2/R3)
- **USB supply** via **ADC heuristic** (no dedicated VBUS pin)

### [Version 20260523](Version20260523/README_EN.md) — second revision

- Battery voltage on **GPIO0** (Pin1/A0/D0) via **1:1 voltage divider** (R2/R3)
- **USB supply:** **VBUS** on **GPIO18** (Pin11/D10) via **asymmetric voltage divider** (R3/R4)
- **Clearance** around the onboard antenna for better RF performance
