<!-- translation-source: README.md -->
<!-- translation-source-blob: 7275a348cebe17c5e8549226a54e9b4d4ef2db75 -->

[↓ Wechsel zu Deutsch](README.md)

# Integration Templates and Configurations

Templates for connecting Gas-O-Meter2 to external systems.

## Structure

```text
integration_templates/
├── zigbee2mqtt/
│   ├── README.md
│   ├── external_converters/gas-o-meter2.js
│   └── device_icons/gas-o-meter2.png
├── nodered/
│   ├── README.md
│   └── gas-o-meter2-ble-flow.json
└── README.md
```

## Usage

### [Zigbee2MQTT](./zigbee2mqtt/README_EN.md)

1. Copy `zigbee2mqtt/external_converters/gas-o-meter2.js` to `<data_path>/external_converters/`
2. Restart Zigbee2MQTT (log: `Loaded external converter: gas-o-meter2`)
3. ESP: `transfer_mode` Zigbee, start pairing → Z2M **Permit join**

Details, exposes, and troubleshooting: [zigbee2mqtt/README_EN.md](./zigbee2mqtt/README_EN.md)

### [Node-RED (BLE)](./nodered/README_EN.md)

1. Import `nodered/gas-o-meter2-ble-flow.json` into Node-RED
2. Adjust BLE config (MAC), MQTT broker, and **PRESETS**
3. Deploy

Details: [nodered/README_EN.md](./nodered/README_EN.md)

## Notes

- Adapt templates to the local installation (paths, broker, topics)
- **Zigbee** and **BLE→MQTT** are separate transfer paths; do not use the same MQTT topics in parallel without coordination
