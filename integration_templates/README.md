# Integration Templates und Konfigurationen

Dieses Verzeichnis enthält Templates und Konfigurationsdateien für die Integration des Gas-O-Meter2 mit externen Systemen.

## Struktur

```text
integration_templates/
├── zigbee2mqtt/          # Zigbee2MQTT Device Definitions
│   ├── README.md
│   └── gas-o-meter2.yaml
├── nodered/              # Node-RED Flows und Templates
│   ├── README.md
│   └── gas-o-meter2-ble-flow.json
└── README.md            # Diese Datei
```

## Verwendung

## [Zigbee2MQTT](./zigbee2mqtt/README.md)

1. Kopiere `zigbee2mqtt/gas-o-meter2.yaml` nach:

   ```text
   <zigbee2mqtt-installation>/data/devices/custom/gas-o-meter2.yaml
   ```

2. Starte Zigbee2MQTT neu

3. Device wird automatisch erkannt (wenn `model_id` im ESP-Code übereinstimmt)

## [Node-RED (BLE)](./nodered/README.md)

1. Importiere `nodered/gas-o-meter2-ble-flow.json` in Node-RED
2. Passe die Konfiguration an (BLE-Device-Name, etc.)
3. Flow aktivieren

## Hinweise

- Diese Dateien sind **Templates** und müssen an die jeweilige Installation angepasst werden
- Pfade und Konfigurationen können je nach Setup variieren
- Siehe jeweilige README-Dateien in den Unterverzeichnissen für Details
