# Node-RED BLE Flow Template

Node-RED Flow Template für BLE-Datenübertragung vom Gas-O-Meter2.

## Installation

1. Öffne Node-RED UI
2. Klicke auf Menü (☰) → Import
3. Wähle `gas-o-meter2-ble-flow.json` aus
4. Passe die Konfiguration an:
   - BLE-Device-Name (Standard: "Gas-O-Meter2")
   - MQTT-Topic (falls verwendet)
   - Datenbank-Integration (optional)

## Voraussetzungen

- Node-RED mit folgenden Nodes:
  - `node-red-contrib-noble-bluetooth` (BLE-Support)
  - `node-red-node-sqlite` (optional, für Datenbank)
  - `node-red-contrib-influxdb` (optional, für InfluxDB)

Installation:
```bash
cd ~/.node-red
npm install node-red-contrib-noble-bluetooth
npm install node-red-node-sqlite
npm install node-red-contrib-influxdb
```

## Flow-Struktur

Der Flow enthält:
1. **BLE-Scanner** - Sucht nach Gas-O-Meter2
2. **Data Parser** - Parst BLE-Charakteristiken
3. **MQTT Publisher** (optional) - Sendet Daten an MQTT
4. **Database Writer** (optional) - Speichert in Datenbank
5. **Dashboard** (optional) - Visualisierung

## Anpassungen

- BLE-Device-Name ändern
- MQTT-Topics anpassen
- Datenbank-Integration konfigurieren
- Dashboard-Widgets anpassen

## Hinweise

- BLE wird erst implementiert, wenn Transfer-Mode "ble" aktiviert ist
- Flow ist ein Template und muss an die jeweilige Installation angepasst werden
- Siehe `docs/TRANSFER_MODULE_IMPLEMENTATION.md` für Implementierungs-Status
