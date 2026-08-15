<!-- translation-source: README.md -->
<!-- translation-source-blob: 2676b610df5a94d19a52f675fa72957313422852 -->

[↓ Wechsel zu Deutsch](README.md)

# Gas-O-Meter2

[![Release](https://img.shields.io/github/v/release/sneumeister/Gas-o-Meter2)](https://github.com/sneumeister/Gas-o-Meter2/releases)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](./LICENSE)
[![ESP32-C6](https://img.shields.io/badge/MCU-ESP32--C6-red)](https://www.espressif.com/en/products/socs/esp32-c6)
[![pioarduino](https://img.shields.io/badge/platform-pioarduino-orange)](https://github.com/pioarduino/platform-espressif32)
[![ESP-IDF](https://img.shields.io/badge/framework-ESP--IDF%205.5-green)](https://docs.espressif.com/projects/esp-idf/)

ESP32-C6 gas meter monitoring with **LP-Core pulse counting**, battery-powered deep sleep, and selectable transfer modes (WiFi/MQTT, BLE, Zigbee).

Custom carrier PCB with a **TPL5110** microtimer for clean reed pulses, optional 3D-printed housing, and a web interface for configuration.

![Status-Übersicht](pictures/status_small.png)
![Gas-o-meter2_angebaut](pictures/gaszaehler.png)

## Features

- **TPL5110** — debounces the reed contact and provides a defined counter pulse (~3.5 s); compensates for the missing Schmitt-trigger input on the ESP32-C6
- **LP-Core (ULP)** — pulse counting on the low-power core (`ulp/ulp_main.c`); the HP core runs only on wake-up for data transfer
- **Deep sleep** — configurable wake-up interval via `wakeup_minutes` in the config
- **NVS ring buffer** — counter is saved only on **low battery** (< 30% / < 3.57V); at ≥ 30% the value stays in RTC RAM (also with USB/VBUS) to reduce flash wear
- **Web interface** — status and configuration via LittleFS (`data/index.html`, `config.html`); not automatically available during **timer wake-up** operation
- **Button A** — web UI and WiFi only after wake-up via **Button A** on the board; pure timer wake-up transfers data and returns to deep sleep immediately
- **Button B** — currently **unused**
- **Transfer modes** — `none`, `mqtt`, `ble`, or `zigbee` (one mode active)

## Hardware

- **MCU:** [Seeed Studio XIAO ESP32-C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/) on a custom carrier PCB
- **Sensors:** reed switch for gas meter pulses, TPL5110, battery and USB detection
- **Documentation:**
  - [KiCAD boards](KiCAD-PCB/README_EN.md) — schematic, BOM, 3D view, and board revisions
  - [Housing / 3D print](CAD-housing/README_EN.md) — STEP files, Creo sources, and photos

The firmware revision (`BOARD_VERSION_ID`, see [Build & Flash](#build--flash)) only controls **USB detection**: default `PCB_20251022` also runs on the newer board (revision 20260523), but uses the ADC heuristic instead of VBUS on GPIO18. For dedicated VBUS detection on the second revision: `PCB_20260523`. Board details: [KiCAD overview](KiCAD-PCB/README_EN.md).

## Transfer modes

| Mode | Description | Integration |
| ---- | ----------- | ----------- |
| `mqtt` | WiFi + MQTT (+ optional Home Assistant autodiscovery) | NTP time sync, `transfer_mqtt.cpp` |
| `ble` | NimBLE, JSON notify on characteristic 0xFFF1 | [Node-RED flow](integration_templates/nodered/README_EN.md) |
| `zigbee` | Zigbee end device (metering + battery cluster) | [Zigbee2MQTT converter](integration_templates/zigbee2mqtt/README_EN.md) |
| `none` | Local capture only; no external transfer | — |

External systems entry point: [integration_templates/README_EN.md](integration_templates/README_EN.md)

## Battery protection & wake-up

LiPo voltage curve (as stored in firmware; percentage is linearly interpolated between points):

| Voltage | % | Without USB (battery) | With USB |
| ------- | - | --------------------- | -------- |
| ≥ 4.02V | 100% | Normal: transfer, counter in RTC RAM | Normal: transfer, counter in RTC RAM (no NVS) |
| 3.92V | 80% | as above | as above |
| 3.72V | 50% | as above | as above |
| 3.57V | 30% | From here or below: low-battery report; counter to NVS | as left |
| 3.42V | 20% | Below: immediate deep sleep, **no** transfer; timer continues until 3.15V | Protection **off** → transfer as normal |
| ≤ 3.15V | 0% | Timer off; only **Button A** wakes | Timer stays on |

**Returning to normal operation** (timer was off): USB alone does **not** wake the device. Connect USB and press **Button A** (starts web UI; wait for web timeout) — then deep sleep with timer.

**USB detection by board:**

- **20251022:** USB = measured voltage < 2.0V. Between 2.0V and 3.42V counts as *no* USB → battery protection applies.
- **20260523:** USB via VBUS pin, independent of battery voltage (firmware environment `PCB_20260523`).

The LP core keeps counting pulses while the chip is powered. **Button B** has no function.

## Software overview

| Component | Path / module | Role |
| --------- | ------------- | ---- |
| HP core | `src/main_idf.cpp` | Boot, config, HTTP server, ADC, deep sleep, transfer orchestration |
| LP core | `ulp/ulp_main.c` | Reed pulse counting on GPIO2 |
| Transfer | `src/transfer.cpp`, `transfer_{mqtt,ble,zigbee}.cpp` | Mode dispatcher and protocol stacks |
| Network | `src/wifi_manager.cpp`, `src/time_sync.cpp` | WiFi STA/AP, mDNS, NTP |
| Persistence | `data/` (LittleFS), `pulse_nv` (NVS) | Web UI, `config.json`, counter ring buffer |
| Version | `include/version.h` | `Gas-O-Meter2` v1.0.1 |

**Typical sequence:** TPL5110 wakes ESP → LP core provides counter → HP core reads ADC/config → optional transfer → deep sleep.

## Development environment

| Component | Version / source |
| --------- | ---------------- |
| IDE | VS Code or **Cursor** with PlatformIO extension |
| PlatformIO | [pioarduino/platform-espressif32](https://github.com/pioarduino/platform-espressif32) (stable zip in `platformio.ini`) |
| Framework | **ESP-IDF 5.5.1** (`sdkconfig.defaults`) |
| Board | `seeed_xiao_esp32c6` |
| Extension (recommended) | `pioarduino.pioarduino-ide` (`.vscode/extensions.json`) |
| IntelliSense | `cpptools` (`.vscode/settings.json`) |

**Requirements:** PlatformIO CLI, Git, USB drivers for XIAO ESP32-C6.

## Build & Flash

Default environment in [`platformio.ini`](platformio.ini): `PCB_20251022`

| Environment | USB detection | Command |
| ----------- | ------------- | ------- |
| `PCB_20251022` | ADC heuristic (default; runs on both board revisions) | `pio run` / `pio run -t upload` |
| `PCB_20260523` | VBUS on GPIO18 (useful only on revision 20260523) | `pio run -e PCB_20260523 -t upload` |
| `TPL_test` | — (hardware test TPL5110/reed on GPIO2, without LP core/WiFi) | `pio run -e TPL_test -t upload` |

**TPL_test:** serial `pio device monitor -e TPL_test` (115200). Open the monitor **before** reset or restart the board afterward (USB-CDC). Edges on GPIO2 with µs timestamps and interval `dt`; heartbeat every 2 s. **TPL function test:** briefly tap reed → one LOW ~3–4 s, then HIGH; permanently bridge reed → only **one** LOW that lasts until the bridge is opened (no short HIGH gaps). **Magnet test:** trigger the reed switch with a magnet (typical sensitivity/distance). Details: [`src/main_TPL_test.cpp`](src/main_TPL_test.cpp).

`BOARD_VERSION_ID` in `include/hardware.h` selects ADC-threshold vs VBUS-GPIO USB detection — not a hard board lock.

**First install:**

1. Clone the repository
2. Create `data/config.json` from [`data/config.json_example`](data/config.json_example)
3. Flash firmware: `pio run -t upload` (default); optional `-e PCB_20260523` for VBUS detection on revision 20260523
4. Flash filesystem: `pio run -t uploadfs` (web UI and default config)
5. Monitor serial: `pio device monitor` (115200 baud)

**Partitions** (`partitions.csv`): factory app, `pulse_nv` (NVS ring), Zigbee storage, LittleFS.

### Web flash and releases

Without PlatformIO, install firmware via the
[Gas-O-Meter2 Web Flasher](https://sneumeister.github.io/Gas-o-Meter2/)
(Firefox 151+ on desktop, Chrome, or Edge; USB data cable required). On first
use Firefox installs a site-specific permission extension; no extra third-party
extension is needed. Both PCB versions and `TPL_test` are supported.

- **Complete:** first install or a release marked **Breaking** in the notes;
  PCB Complete resets persistent config and counter/Zigbee data
- **Firmware:** program code only
- **LittleFS:** web UI and default config only; overwrites the current device
  configuration

For firmware/LittleFS partial updates, never choose **“Erase device”** in the
ESP Web Tools dialog.

Release tags `v*` build and publish these files automatically. Maintainer steps
are in [RELEASING.md](RELEASING.md).

## Configuration

Two options:

1. **Web UI** after wake-up via **Button A** ([Web UI guide](README_WEBUI_EN.md))
2. **`data/config.json`** — edit manually or save via the web UI

**Note on continuous operation:** On periodic timer wake-up (battery), the device wakes briefly, may transfer data, and returns to deep sleep — **without** the web frontend. Status and configuration are only available after manual wake-up via **Button A** (WiFi STA or captive portal in AP mode).

Important config keys (full list in `config.json_example`):

| Group | Keys |
| ----- | ---- |
| Device | `hostname`, `adminpass` |
| WiFi | `wifiCredentials` (max. 2 networks) |
| Timing | `wakeup_minutes`, `transfer_interval_x` |
| Transfer | `transfer_mode` (`none` / `mqtt` / `ble` / `zigbee`) |
| MQTT | `mqtt_host`, `mqtt_port`, `mqtt_username`, `mqtt_password`, `mqtt_main_topic`, `mqtt_ha_autodiscovery`, `ntp_server` |
| Calibration / TX | `adc_voltage_multiplier`, `wifi_tx_power_dbm`, `ble_tx_power_dbm`, `zigbee_tx_power_dbm` |

## Project structure

```text
gas-o-meter2/
├── src/                    # Firmware (ESP-IDF)
├── ulp/                    # LP-Core pulse counting
├── data/                   # LittleFS web UI + config.json
├── include/                # Headers (hardware, transfer, version)
├── KiCAD-PCB/              # Board documentation
├── CAD-housing/            # Housing STEP/Creo/images
├── integration_templates/  # Zigbee2MQTT, Node-RED
├── pictures/webui/         # Screenshots for README_WEBUI.md
├── scripts/                # Release build and artifact verification
├── web-flasher/            # GitHub Pages install page (no binaries)
├── RELEASING.md            # Maintainer release guide
└── platformio.ini          # Build environments
```

## License

Gas-O-Meter2 is published under the [Apache License 2.0](./LICENSE).
Copyright and attribution notices are in [`NOTICE`](./NOTICE). Third-party
licenses are documented in [`THIRD_PARTY_NOTICES.md`](./THIRD_PARTY_NOTICES.md).

Apache 2.0 was chosen to avoid possible conflicts between strong copyleft and
the additional binary terms of the separately licensed ZBOSS component. ZBOSS
and all other third-party components keep their own licenses.
