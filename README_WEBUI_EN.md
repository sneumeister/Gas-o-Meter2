<!-- translation-source: README_WEBUI.md -->
<!-- translation-source-blob: 8d73e32c50d9de1c6fe0f405ec789419438e2e91 -->

# Web UI (Gas-O-Meter2)

[Wechsel zu Deutsch](README_WEBUI.md)

After a **manual wake-up via Button A**, the device serves a small HTTP frontend (LittleFS under `data/`). In **timer continuous operation** (battery) it wakes briefly, may transfer data, and returns to deep sleep — **without** a web server.

## Access and URLs

| Situation | URL | Basic Auth |
| --------- | --- | ---------- |
| Home WiFi (STA) | `http://<hostname>.local/` or `http://<IP>/` | Status: no, Config: yes |
| Setup hotspot (AP) | `http://10.0.0.1/` (captive portal) | Status: no, Config: yes |

- **Hostname** comes from `config.json` (`hostname`, default in [`data/config.json_example`](data/config.json_example): `gas-o-meter2`).
- **mDNS** works in STA mode if the network resolves `.local` names.
- In AP mode, connect to the device's open WLAN (`Gas-O-Meter2` or `<hostname>`) and open `10.0.0.1`.

### Basic Auth for `/config`

The configuration page is protected:

- **Username:** always `admin`
- **Password:** value of `adminpass` in `config.json` (on first install e.g. `AdminPasswort` from the example)

The browser prompts for these credentials on the first visit to `/config`.

## Web timeout (deep sleep)

While the web UI is active, the device stays awake. After **5 minutes** without web-server access it enters deep sleep (`WIFI_WAIT_FOR_SLEEP` in [`include/hardware.h`](include/hardware.h)). The config page sends a ping every 2 minutes to keep the session alive.

## Status page (`/`)

![Status-Übersicht](pictures/webui/status.png)

The home page shows the **counter reading** (LP-core pulses), system info, and action buttons:

- **Aktualisieren** — reload page
- **Config** — open configuration (Basic Auth)
- **Deep-Sleep** — send device to deep sleep immediately
- **Reboot** — restart

### Parameter table

![Parameter einblenden](pictures/webui/status-parameters.png)

**Parameter einblenden** shows firmware version, board version, WiFi SSID, transfer mode, battery voltage, USB status, time sync, and more. The admin password is shown only as `***`.

### Correct counter reading

![Zählerstand korrigieren](pictures/webui/status-counter.png)

**Zählerstand korrigieren** adjusts the displayed value to the physical gas meter via direct numeric input, slider, or +/-. Switch between integer and fractional digits with Tab, period/comma, or arrow keys. **Übernehmen** saves the value (NVS/RTC).

## Configuration page (`/config`)

The configuration page is protected (browser prompt):

- **Username:** always `admin`
- **Password:** value of `adminpass` in `config.json` (on first install e.g. `AdminPasswort` from the example)

![Konfiguration – Übersicht](pictures/webui/config.png)

Main sections:

| Section | Content |
| ------- | ------- |
| Device | Hostname, change admin password |
| WiFi credentials | Up to 2 networks, scan **WLAN in der Nähe**, TX power |
| Timing | Wake-up interval (`wakeup_minutes`) |
| Transfer | Mode `none` / `mqtt` / `ble` / `zigbee`, interval, ADC multiplier, NTP |
| Actions | **Konfig Neuladen**, **Konfig Speichern**, **Reboot**, **Zurück** |

After **Konfig Speichern**, `config.json` is written to LittleFS; a **Reboot** fully applies many settings.

### ZigBee settings

![ZigBee-Konfiguration](pictures/webui/config-zigbee.png)

When `transfer_mode` is **zigbee** and saved, the **ZigBee-Einstellungen** panel appears with TX power and **ZigBee-Status** (pairing, network address). Pairing and factory reset are only possible in **STA mode** (connected to your WLAN), not on the setup hotspot.

### MQTT settings

![MQTT-Konfiguration (Beispieldaten)](pictures/webui/config-mqtt.png)

With saved mode **mqtt**, the **MQTT-Einstellungen** panel opens (host, port, credentials, main topic, Home Assistant Auto-Discovery, **MQTT-Server testen**). The screenshot shows **example values**; the device shows your real `config.json` entries.

MQTT test and transfer require a **STA connection** to the home WLAN (not AP only). Without a real MQTT host, a dummy host is stored and MQTT transfer is skipped.

## Typical workflows

### First setup

1. Wake the device with **Button A**.
2. Connect to the setup hotspot or — if already configured — open `http://<hostname>.local/`.
3. Under **Config** (`admin` + `adminpass`) enter WiFi data and **Konfig Speichern**.
4. **Reboot**; then check the status page on the home WLAN.

### Change transfer mode

1. In **Datenübertragung** select the desired mode.
2. Configure the mode-specific panel (visible only for the **saved** mode; after a dropdown change without save, detail panels stay hidden).
3. **Konfig Speichern** and **Reboot**.

### Match counter to gas meter

1. Open the status page.
2. **Zählerstand korrigieren**, type the value or use slider/+/-, **Übernehmen**.

### Pair ZigBee

1. Set `transfer_mode`: `zigbee`, save, reboot.
2. On the home WLAN open Config, expand **ZigBee-Einstellungen**.
3. Start pairing via Zigbee2MQTT / coordinator; check status in the table.

Integration details: [integration_templates/zigbee2mqtt/README_EN.md](integration_templates/zigbee2mqtt/README_EN.md)

## Limits and notes

- Web UI only after **Button A wake-up**, not on pure timer wake-up on battery.
- **ZigBee** and **MQTT** (including test) need **WiFi STA**; in AP mode only configuration, no pairing/transfer.
- Passwords (WLAN, MQTT, admin) are stored in clear text in `config.json` — keep LittleFS reachable only on the local network.
- Full config keys: [`data/config.json_example`](data/config.json_example) and the **Configuration** section in [README_EN.md](README_EN.md).
