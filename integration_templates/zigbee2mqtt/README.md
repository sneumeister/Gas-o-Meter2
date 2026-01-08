# Zigbee2MQTT Device Definition

Custom Device Definition für Gas-O-Meter2 in Zigbee2MQTT.

## Installation

1. Kopiere `gas-o-meter2.yaml` nach:
   ```
   <zigbee2mqtt-installation>/data/devices/custom/gas-o-meter2.yaml
   ```
   
   **Typische Pfade:**
   - Docker: `/app/data/devices/custom/`
   - Native: `~/.zigbee2mqtt/data/devices/custom/`
   - Home Assistant Add-on: `/config/zigbee2mqtt/data/devices/custom/`

2. Starte Zigbee2MQTT neu:
   ```bash
   # Docker
   docker restart zigbee2mqtt
   
   # Native
   systemctl restart zigbee2mqtt
   
   # Home Assistant Add-on
   # Über Home Assistant UI: Add-on neu starten
   ```

3. Prüfe Logs, ob Device Definition geladen wurde:
   ```
   Loaded device definition: gas-o-meter2
   ```

## Voraussetzungen

- ESP32C6 muss `model_id = "Gas-O-Meter2"` im ZigBee-Stack konfiguriert haben
- Siehe `src/transfer_zigbee.cpp` für die entsprechende Konfiguration

## Verwendung

Nach dem Pairing erscheint das Device in Zigbee2MQTT mit:
- `gas_counter` - Gas-Zählerstand (pulses)
- `battery` - Akku-Ladezustand (%)
- `firmware_version` - Firmware-Version (String)

## Anpassungen

Die Datei kann angepasst werden für:
- Zusätzliche Exposes (z.B. Temperatur, etc.)
- Custom Cluster-Definitionen
- Reporting-Intervalle
- Home Assistant Integration

Siehe [Zigbee2MQTT Device Definition Guide](https://www.zigbee2mqtt.io/advanced/zigbee/01_how_to_create_support_for_new_devices.html) für Details.
