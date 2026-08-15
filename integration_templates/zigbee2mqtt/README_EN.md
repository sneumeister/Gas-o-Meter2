<!-- translation-source: README.md -->
<!-- translation-source-blob: 99d9d46d4a4ae9e72ad1635c8481870b0057e7cc -->

# Zigbee2MQTT – External Converter

[Wechsel zu Deutsch](README.md)

External converter for Gas-O-Meter2 in Zigbee2MQTT (`external_converters/gas-o-meter2.js`).

Firmware uses **Simple Metering** (0x0702) for gas and **Power Configuration** (0x0001) for the battery. The converter maps ZCL attributes to MQTT/HA exposes (see below).

---

## Project structure

```text
integration_templates/zigbee2mqtt/
├── external_converters/
│   └── gas-o-meter2.js       ← copy to <data_path>/external_converters/
├── device_icons/
│   └── gas-o-meter2.png      ← optional to <data_path>/device_icons/
└── README.md
```

---

## Quick start

1. **Copy converter:**

   ```bash
   cp external_converters/gas-o-meter2.js <data_path>/external_converters/
   ```

2. **Restart Zigbee2MQTT**

3. **Check log:** `Loaded external converter: gas-o-meter2`

4. **Pairing:** ESP web UI → “ZigBee Pairing starten” → enable Z2M **Permit join**

---

## Installation (detailed)

### Step 1: Find `data_path`

`configuration.yaml` lives **inside** `data_path`:

| Installation | Typical `data_path` |
| ------------ | ------------------- |
| Docker | `/app/data/` |
| Native | `~/.zigbee2mqtt/data/` |
| Home Assistant Add-on | `/config/zigbee2mqtt/data/` |

`external_converters/` sits **next to** `configuration.yaml`:

```text
<data_path>/
├── configuration.yaml
├── external_converters/
│   └── gas-o-meter2.js
└── device_icons/          ← optional
    └── gas-o-meter2.png
```

### Step 2: Install external converter

```bash
mkdir -p <data_path>/external_converters/
cp gas-o-meter2.js <data_path>/external_converters/
```

**Examples:**

```bash
# Docker
docker cp gas-o-meter2.js zigbee2mqtt:/app/data/external_converters/

# Native
mkdir -p ~/.zigbee2mqtt/data/external_converters/
cp gas-o-meter2.js ~/.zigbee2mqtt/data/external_converters/
```

**Note:** In the repo the file is **`gas-o-meter2.js`** (CommonJS, `module.exports`). Zigbee2MQTT loads `.js` and `.mjs` from `external_converters/`.

### Step 3: Restart Zigbee2MQTT

Docker: `docker restart zigbee2mqtt` · Native: `systemctl restart zigbee2mqtt` · HA add-on: restart in the add-on UI.

### Step 4: Verify installation

```bash
docker logs zigbee2mqtt 2>&1 | grep -i gas-o-meter2
# Expect e.g.: Loaded external converter: gas-o-meter2
```

After pairing, configure log lines such as `[gas-o-meter2 configure] configure bind OK: …` appear (reporting is set in the converter).

### Step 5 (optional): Device icon

Copy a PNG (e.g. 512×512) to `<data_path>/device_icons/gas-o-meter2.png`.

The converter has **no** `icon` in `meta` by default. Optionally add in `gas-o-meter2.js`:

```javascript
meta: {
    battery: {type: 'battery'},
    configureKey: 6,
    icon: 'device_icons/gas-o-meter2.png',
},
```

Then restart Z2M (or raise `configureKey` for another configure run).

---

## Firmware prerequisites

In ESP code (`include/zigbee_config.h`, `src/transfer_zigbee.cpp`):

| ZCL / Z2M | Value |
| --------- | ----- |
| Model ID | `Gas-O-Meter2` (`ZIGBEE_MODEL_ID`) |
| Manufacturer | `Custom` (`ZIGBEE_MANUFACTURER_NAME`) |
| Zigbee model (converter) | `zigbeeModel: ['Gas-O-Meter2']`, `model: 'gas-o-meter2'`, `vendor: 'Custom'` |

Gas scale: firmware **Multiplier=1**, **Divisor=100** → raw / 100 = m³ in the converter.

---

## Usage

### Device pairing

1. ESP: web UI → **ZigBee Pairing starten**
2. Z2M: enable **Permit join** (~60 s)
3. Device appears with **Model** `gas-o-meter2`, **Vendor** `Custom`

### MQTT exposes (after pairing)

**State keys** published by the converter (friendly name may differ):

| Expose | Meaning | Source (Zigbee) |
| ------ | ------- | --------------- |
| **`gas`** | Gas meter reading [m³] | `seMetering` / `currentSummDelivered` ÷ 100 |
| **`battery`** | Charge [%] | `genPowerCfg` / `batteryPercentageRemaining` ÷ 2 |
| **`battery_voltage`** | Voltage [V] | `genPowerCfg` / `batteryVoltage` (100 mV → V) |
| **`battery_low`** | Low battery (boolean) | `genPowerCfg` / `batteryAlarmState` bit 0 |

**Firmware version:** Firmware writes `swBuildId` (`PROJECT_VERSION`) and `appVersion` (uint8, first version digit) to the **Basic cluster**. That is visible in the **Z2M device overview** — the converter does **not** create an MQTT sensor `firmware_version` (unlike the MQTT/BLE path).

### Reporting (on pairing, in the converter)

| Attribute | min | max | change |
| --------- | --- | --- | ------ |
| `currentSummDelivered` (gas) | 300 s | 3600 s | 0 |
| `batteryPercentageRemaining` | (Z2M preset) | | |
| `batteryVoltage` | (Z2M preset) | | |
| `batteryAlarmState` | 3600 s | 86400 s | 0 |

`version` in the definition (currently `0.0.3`, format `0.0.<patch>`): raising the patch value makes Z2M call `configure()` again (log: `definition v0.0.3`). Not the firmware version.

`configureKey` in `meta` (currently `6`): changing it makes Z2M call `configure()` again — without re-pairing. On configure/reconfigure the converter reads `genBasic`/`swBuildId` and sets `device.softwareBuildID` (About UI “Firmware-ID”). Device must be awake.

### Home Assistant

Via the Zigbee2MQTT integration, entities typically appear for the exposes, e.g.:

- `sensor.<friendly_name>_gas`
- `sensor.<friendly_name>_battery`
- `sensor.<friendly_name>_battery_voltage`
- `binary_sensor.<friendly_name>_battery_low`

Exact entity IDs depend on the **friendly name** in Z2M.

---

## Customization

- Exposes / reporting: `external_converters/gas-o-meter2.js`
- After changes: restart Z2M; raise `configureKey` in `meta` if needed
- [External converters (Z2M docs)](https://www.zigbee2mqtt.io/advanced/more/external_converters.html)

---

## Troubleshooting

### Device not recognized

- Model ID **`Gas-O-Meter2`**, manufacturer **`Custom`** active on the ESP?
- `gas-o-meter2.js` in `<data_path>/external_converters/` (not only in the repo)?
- Log: `Loaded external converter: gas-o-meter2`
- JavaScript syntax error? → check Z2M log at start

### Device present, but no values

- After pairing: configure logs (`configure bind OK`, `configure reporting OK`)?
- Clusters on ESP: Basic, Power Config, Simple Metering (see firmware logs)
- Sleepy end device: values only after wake-up / report interval

### UNSUPPORTED_ATTRIBUTE / reporting

The converter sets bind and reporting manually in `configure()` (workaround for sleepy EDs). On errors: search Z2M log for `[gas-o-meter2 configure]`; raise `configureKey` and restart Z2M.

---

## Further information

- [Zigbee2MQTT External Converters](https://www.zigbee2mqtt.io/advanced/more/external_converters.html)
- Firmware: `src/transfer_zigbee.cpp`, `include/zigbee_config.h`
