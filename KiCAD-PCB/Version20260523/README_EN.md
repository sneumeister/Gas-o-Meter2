<!-- translation-source: README.md -->
<!-- translation-source-blob: 1e98588e5097565264d01e840f41fa663bcdbc6c -->

[↓ Wechsel zu Deutsch](README.md)

# Gas-O-Meter2 — Board Version 20260523

KiCAD project and bill of materials (BOM) for the second board revision.

- Battery voltage on **GPIO0** (Pin1/A0/D0) via **1:1 voltage divider** (R2/R3)
- **USB supply:** **VBUS** on **GPIO18** (Pin11/D10) via **asymmetric voltage divider** (R3/R4)
- **Clearance** around the onboard antenna for better RF performance

## Schematic

[Schaltplan ESP32C6_gasometer (PDF)](Schaltplan_ESP32C6_gasometer.pdf)

![Schaltplan ESP32C6_gasometer](ESP32C6_gasometer_Schaltplan.png)

## Bill of materials (BOM)

Source: `BOM_ESP32C6_gasometer.xlsx`

| Reference | Qty | Description | Value | Datasheet |
| --------- | --- | ----------- | ----- | --------- |
| J1 | 1 | Generic connector, single row, 01x02 // Reed | JST-XH 1.25 2pin socket | |
| J2 | 1 | Generic connector, single row, 01x02 // Batterie | JST-XH 1.25 2pin plug | |
| R1 | 1 | Resistor_SMD1206 | 8,2k | |
| R2,R3 | 2 | Resistor_SMD1206 | 390k | |
| R4 | 1 | Resistor_SMD1206 | 330k | |
| R5 | 1 | Resistor_SMD1206 | 220k | |
| SW1 | 1 | reed switch | KSK-1A87-1520 | [Datasheet](https://standexdetect.de/wp-content/uploads/sites/8/2025/09/datasheet-reed-switch-series-ksk-1a80.pdf) |
| SW2,SW3 | 2 | Push button switch, generic, two pins | OMRON_B3F-6000 | [Datasheet](https://omronfs.omron.com/en_US/ecb/products/pdf/en-b3f.pdf) |
| U1 | 1 | Timer, Nano Power, SOT-23-6 | TPL5110 | [Datasheet](http://www.ti.com/lit/ds/symlink/tpl5110.pdf) |
| U2 | 1 | | Seeed Studio XIAO ESP32-C6 | |
| J3 | | JST-Conn. / Battery Connector | JST-PH 2.00 2pin socket | |
| | 1 | Li-Polymer Battery 52x34x5,0mm | LiPo 1000mAh 523450 | |

## 3D view

![ESP32C6 PCB 3D](ESP32C6_PCB_3D.png)

## KiCAD export

Current snapshot: [ESP32C6_gasometer-2026-07-11_162129](ESP32C6_gasometer-2026-07-11_162129/)
