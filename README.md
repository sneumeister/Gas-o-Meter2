# Gas-O-Meter2

ESP32C6-basiertes Gaszähler-Monitoring-System mit LP-Core Pulse-Counting.

## Features

- LP-Core basiertes Pulse-Counting (niedriger Stromverbrauch)
- Web-Interface für Status und Konfiguration
- Deep-Sleep für Akku-Betrieb
- NVS Ring-Buffer für Daten-Persistenz

## Hardware

- Board: Seeed Studio XIAO ESP32C6
- Reed-Kontakt für Gaszähler-Pulse

## Installation

1. PlatformIO installieren
2. Repository klonen
3. `data/config.json` aus `data/config.json_example` erstellen
4. WiFi-Credentials in `data/config.json` eintragen
5. `pio run -t upload` ausführen

## Konfiguration

Siehe `data/config.json_example` für Konfigurations-Optionen.

## Lizenz

[Ihre Lizenz hier]
