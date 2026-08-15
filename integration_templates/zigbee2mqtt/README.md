# Zigbee2MQTT – External Converter

[Switch to English](README_EN.md)

External Converter für Gas-O-Meter2 in Zigbee2MQTT (`external_converters/gas-o-meter2.js`).

Die Firmware nutzt **Simple Metering** (0x0702) für Gas und **Power Configuration** (0x0001) für die Batterie. Der Converter mappt die ZCL-Attribute auf die MQTT/HA-Exposes (siehe unten).

---

## Projektstruktur

```text
integration_templates/zigbee2mqtt/
├── external_converters/
│   └── gas-o-meter2.js       ← nach <data_path>/external_converters/ kopieren
├── device_icons/
│   └── gas-o-meter2.png      ← optional nach <data_path>/device_icons/
└── README.md
```

---

## Schnellstart

1. **Converter kopieren:**

   ```bash
   cp external_converters/gas-o-meter2.js <data_path>/external_converters/
   ```

2. **Zigbee2MQTT neu starten**

3. **Log prüfen:** `Loaded external converter: gas-o-meter2`

4. **Pairing:** ESP Web-UI → „ZigBee Pairing starten“ → in Z2M **Permit join** aktivieren

---

## Installation (detailliert)

### Schritt 1: `data_path` finden

Die `configuration.yaml` liegt **im** `data_path`:

| Installation | Typischer `data_path` |
| ------------ | --------------------- |
| Docker | `/app/data/` |
| Native | `~/.zigbee2mqtt/data/` |
| Home Assistant Add-on | `/config/zigbee2mqtt/data/` |

Der Ordner `external_converters/` liegt **daneben** zur `configuration.yaml`:

```text
<data_path>/
├── configuration.yaml
├── external_converters/
│   └── gas-o-meter2.js
└── device_icons/          ← optional
    └── gas-o-meter2.png
```

### Schritt 2: External Converter installieren

```bash
mkdir -p <data_path>/external_converters/
cp gas-o-meter2.js <data_path>/external_converters/
```

**Beispiele:**

```bash
# Docker
docker cp gas-o-meter2.js zigbee2mqtt:/app/data/external_converters/

# Native
mkdir -p ~/.zigbee2mqtt/data/external_converters/
cp gas-o-meter2.js ~/.zigbee2mqtt/data/external_converters/
```

**Hinweis:** Die Datei heißt im Repo **`gas-o-meter2.js`** (CommonJS, `module.exports`). Zigbee2MQTT lädt `.js` und `.mjs` aus `external_converters/`.

### Schritt 3: Zigbee2MQTT neu starten

Docker: `docker restart zigbee2mqtt` · Native: `systemctl restart zigbee2mqtt` · HA Add-on: Neustart im Add-on-UI.

### Schritt 4: Installation prüfen

```bash
docker logs zigbee2mqtt 2>&1 | grep -i gas-o-meter2
# Erwartet u. a.: Loaded external converter: gas-o-meter2
```

Nach dem Pairing erscheinen Configure-Logzeilen wie `[gas-o-meter2 configure] configure bind OK: …` (Reporting wird im Converter gesetzt).

### Schritt 5 (optional): Device-Icon

PNG (z. B. 512×512) nach `<data_path>/device_icons/gas-o-meter2.png` kopieren.

Im Converter ist **kein** `icon` in `meta` voreingestellt. Optional in `gas-o-meter2.js` ergänzen:

```javascript
meta: {
    battery: {type: 'battery'},
    configureKey: 6,
    icon: 'device_icons/gas-o-meter2.png',
},
```

Danach Z2M neu starten (oder `configureKey` erhöhen für erneutes Configure).

---

## Voraussetzungen (Firmware)

Im ESP-Code (`include/zigbee_config.h`, `src/transfer_zigbee.cpp`):

| ZCL / Z2M | Wert |
| --------- | ---- |
| Model ID | `Gas-O-Meter2` (`ZIGBEE_MODEL_ID`) |
| Manufacturer | `Custom` (`ZIGBEE_MANUFACTURER_NAME`) |
| Zigbee-Modell (Converter) | `zigbeeModel: ['Gas-O-Meter2']`, `model: 'gas-o-meter2'`, `vendor: 'Custom'` |

Gas-Skalierung: Firmware **Multiplier=1**, **Divisor=100** → Rohwert / 100 = m³ im Converter.

---

## Verwendung

### Device Pairing

1. ESP: Web-UI → **ZigBee Pairing starten**
2. Z2M: **Permit join** aktivieren (ca. 60 s)
3. Gerät erscheint mit **Model** `gas-o-meter2`, **Vendor** `Custom`

### MQTT-Exposes (nach Pairing)

Vom Converter veröffentlichte **State-Keys** (friendly name kann abweichen):

| Expose | Bedeutung | Quelle (Zigbee) |
| ------ | --------- | ---------------- |
| **`gas`** | Gaszählerstand [m³] | `seMetering` / `currentSummDelivered` ÷ 100 |
| **`battery`** | Ladezustand [%] | `genPowerCfg` / `batteryPercentageRemaining` ÷ 2 |
| **`battery_voltage`** | Spannung [V] | `genPowerCfg` / `batteryVoltage` (100 mV → V) |
| **`battery_low`** | Batterie niedrig (boolean) | `genPowerCfg` / `batteryAlarmState` Bit 0 |

**Firmware-Version:** Die Firmware schreibt `swBuildId` (`PROJECT_VERSION`) und `appVersion` (uint8, erste Versionsziffer) in den **Basic Cluster**. Das ist in der **Z2M-Geräteübersicht** sichtbar – der Converter legt **keinen** MQTT-Sensor `firmware_version` an (anders als beim MQTT/BLE-Pfad).

### Reporting (beim Pairing, im Converter)

| Attribut | min | max | change |
| -------- | --- | --- | ------ |
| `currentSummDelivered` (Gas) | 300 s | 3600 s | 0 |
| `batteryPercentageRemaining` | (Preset Z2M) | | |
| `batteryVoltage` | (Preset Z2M) | | |
| `batteryAlarmState` | 3600 s | 86400 s | 0 |

`version` in der Definition (aktuell `0.0.3`, Format `0.0.<patch>`): bei Erhöhung des Patch-Werts löst Z2M ein erneutes `configure()` aus (Log: `definition v0.0.3`). Nicht die Firmware-Version.

`configureKey` in `meta` (aktuell `6`): bei Änderung ruft Z2M `configure()` erneut auf – ohne Re-Pair. Beim Configure/Rekonfigurieren liest der Converter `genBasic`/`swBuildId` und setzt `device.softwareBuildID` (About-UI „Firmware-ID“). Gerät muss wach sein.

### Home Assistant

Über die Zigbee2MQTT-Integration erscheinen typischerweise Entitäten zu den Exposes, z. B.:

- `sensor.<friendly_name>_gas`
- `sensor.<friendly_name>_battery`
- `sensor.<friendly_name>_battery_voltage`
- `binary_sensor.<friendly_name>_battery_low`

Exakter Entity-ID hängt vom **friendly name** in Z2M ab.

---

## Anpassungen

- Exposes / Reporting: `external_converters/gas-o-meter2.js`
- Nach Änderungen: Z2M neu starten; ggf. `configureKey` in `meta` erhöhen
- [External Converters (Z2M-Doku)](https://www.zigbee2mqtt.io/advanced/more/external_converters.html)

---

## Troubleshooting

### Gerät wird nicht erkannt

- Model ID **`Gas-O-Meter2`**, Manufacturer **`Custom`** am ESP aktiv?
- `gas-o-meter2.js` in `<data_path>/external_converters/` (nicht nur im Repo)?
- Log: `Loaded external converter: gas-o-meter2`
- JavaScript-Syntaxfehler? → Z2M-Log beim Start prüfen

### Gerät da, aber keine Werte

- Nach Pairing: Configure-Logs (`configure bind OK`, `configure reporting OK`)?
- Cluster auf dem ESP: Basic, Power Config, Simple Metering (siehe Firmware-Logs)
- Sleepy End Device: erst nach Wake-up / Report Intervall Werte

### UNSUPPORTED_ATTRIBUTE / Reporting

Der Converter setzt Bind und Reporting manuell in `configure()` (Workaround für Sleepy-EDs). Bei Fehlern: Z2M-Log nach `[gas-o-meter2 configure]` durchsuchen; `configureKey` erhöhen und Z2M neu starten.

---

## Weitere Informationen

- [Zigbee2MQTT External Converters](https://www.zigbee2mqtt.io/advanced/more/external_converters.html)
- Firmware: `src/transfer_zigbee.cpp`, `include/zigbee_config.h`
