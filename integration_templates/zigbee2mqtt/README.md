# Zigbee2MQTT Device Definition

Custom Device Definition für Gas-O-Meter2 in Zigbee2MQTT.

## Projektstruktur

Die Dateien in diesem Verzeichnis sind wie folgt organisiert:

```
integration_templates/zigbee2mqtt/
├── external_converters/
│   └── gas-o-meter2.mjs      ← External Converter (zu kopieren nach <data_path>/external_converters/)
├── device_icons/
│   └── gas-o-meter2.png      ← Device Icon (zu kopieren nach <data_path>/device_icons/)
└── README.md                  ← Diese Dokumentation
```

**Installation:**
- Kopiere `external_converters/gas-o-meter2.mjs` nach `<data_path>/external_converters/`
- Optional: Kopiere `device_icons/gas-o-meter2.png` nach `<data_path>/device_icons/`

## Schnellstart (External Converter - Empfohlen)

1. **JavaScript-Datei kopieren:**
   ```bash
   # Von diesem Projekt-Verzeichnis:
   cp external_converters/gas-o-meter2.mjs <data_path>/external_converters/
   
   # Oder wenn du bereits im external_converters Verzeichnis bist:
   cp gas-o-meter2.mjs <data_path>/external_converters/
   ```

2. **Zigbee2MQTT neu starten**

3. **Logs prüfen:** `Loaded external converter: gas-o-meter2`

4. **Device pairen:** ESP32C6 in Pairing-Modus → Zigbee2MQTT "Permit join" aktivieren

---

## Installation (Detailliert)

### Schritt 1: data_path finden

Der `data_path` ist das Verzeichnis, in dem Zigbee2MQTT seine Daten speichert. Die `configuration.yaml` liegt **INNERHALB** des `data_path` Verzeichnisses.

**Standard-Pfade (wenn nicht anders konfiguriert):**

- **Docker:** `/app/data/` → `configuration.yaml` liegt bei `/app/data/configuration.yaml`
- **Native Installation:** `~/.zigbee2mqtt/data/` → `configuration.yaml` liegt bei `~/.zigbee2mqtt/data/configuration.yaml`
- **Home Assistant Add-on:** `/config/zigbee2mqtt/data/` → `configuration.yaml` liegt bei `/config/zigbee2mqtt/data/configuration.yaml`

**So findest du den data_path:**

1. **Methode 1: Über configuration.yaml**
   - Die `configuration.yaml` liegt im `data_path` Verzeichnis
   - Finde die `configuration.yaml` → das übergeordnete Verzeichnis ist der `data_path`
   - Beispiel: Wenn `configuration.yaml` bei `/app/data/configuration.yaml` liegt → `data_path = /app/data/`

2. **Methode 2: Über Umgebungsvariable**
   - Prüfe die Umgebungsvariable `ZIGBEE2MQTT_DATA` (falls gesetzt)
   - Diese überschreibt den Standard-Pfad

3. **Methode 3: Standard-Pfade**
   - Falls keine spezielle Konfiguration vorhanden ist, verwende die Standard-Pfade (siehe oben)

**Verzeichnisstruktur:**
```
<zigbee2mqtt-installation>/  (z.B. /app/, ~/.zigbee2mqtt/, /config/zigbee2mqtt/)
└── data/  ← Das ist der data_path
    ├── configuration.yaml  ← Liegt IM data_path!
    ├── database.db
    ├── devices/
    │   └── custom/  ← Für YAML-Definitionen
    ├── device_icons/
    └── external_converters/  ← Liegt IM data_path (neben configuration.yaml)!
        └── gas-o-meter2.mjs
```

### Schritt 2: External Converter installieren (Empfohlen)

**Methode: External Converter (JavaScript - Modern)**

Die Datei `gas-o-meter2.mjs` ist ein moderner External Converter, der die neuesten Zigbee2MQTT-Features nutzt.

**Installation:**

1. **external_converters Ordner erstellen** (falls nicht vorhanden):
   ```bash
   mkdir -p <data_path>/external_converters/
   ```
   
   **Wichtig:** Der `external_converters` Ordner liegt **IM** `data_path` (neben der `configuration.yaml`):
   - Wenn `data_path = /app/data/` → `external_converters` bei `/app/data/external_converters/`
   - Wenn `data_path = ~/.zigbee2mqtt/data/` → `external_converters` bei `~/.zigbee2mqtt/data/external_converters/`
   - Wenn `data_path = /config/zigbee2mqtt/data/` → `external_converters` bei `/config/zigbee2mqtt/data/external_converters/`
   
   **Hinweis:** Das `data` Verzeichnis enthält sowohl die `configuration.yaml` als auch den `external_converters` Ordner.

2. **Datei kopieren:**
   
   Der `external_converters` Ordner liegt **IM** `data_path`:
   ```bash
   # Allgemein: IM data_path
   cp gas-o-meter2.mjs <data_path>/external_converters/
   ```
   
   **Konkrete Beispiele:**
   ```bash
   # Docker (wenn data_path = /app/data/)
   docker cp gas-o-meter2.mjs zigbee2mqtt:/app/data/external_converters/
   
   # Native (wenn data_path = ~/.zigbee2mqtt/data/)
   mkdir -p ~/.zigbee2mqtt/data/external_converters/
   cp gas-o-meter2.mjs ~/.zigbee2mqtt/data/external_converters/
   
   # Home Assistant (wenn data_path = /config/zigbee2mqtt/data/)
   mkdir -p /config/zigbee2mqtt/data/external_converters/
   cp gas-o-meter2.mjs /config/zigbee2mqtt/data/external_converters/
   ```
   
   **Wichtig:** 
   - `configuration.yaml` liegt **IM** `data_path`: `<data_path>/configuration.yaml`
   - `external_converters` liegt **IM** `data_path`: `<data_path>/external_converters/`

3. **Zigbee2MQTT neu starten** (siehe Schritt 3)

**Vorteile der External Converter Methode:**
- ✅ Modernes JavaScript-Format mit ES6 Modules
- ✅ Nutzt moderne Extends (z.B. `battery()`)
- ✅ Bessere TypeScript-Unterstützung
- ✅ Einfacher zu warten und zu erweitern
- ✅ Wird automatisch von Zigbee2MQTT geladen

### Schritt 2 (Alternative): YAML Device Definition (Legacy)

**Methode: YAML Device Definition (Legacy - Alternative)**

Die Datei `gas-o-meter2.yaml` ist eine Legacy-Definition im YAML-Format.

**Installation:**

1. **custom Ordner erstellen** (falls nicht vorhanden):
   ```bash
   mkdir -p <zigbee2mqtt-data-path>/devices/custom/
   ```

2. **Datei kopieren:**
   ```bash
   cp gas-o-meter2.yaml <zigbee2mqtt-data-path>/devices/custom/
   ```
   
   **Typische Installationspfade:**

   #### Docker Installation:
   ```bash
   # Pfad im Container: /app/data/devices/custom/
   # Datei von Host-System kopieren:
   docker cp gas-o-meter2.yaml zigbee2mqtt:/app/data/devices/custom/
   
   # Oder Volume-Mount verwenden (wenn konfiguriert):
   # Host-Pfad: /pfad/zu/zigbee2mqtt/data/devices/custom/
   cp gas-o-meter2.yaml /pfad/zu/zigbee2mqtt/data/devices/custom/
   ```

   #### Native Installation (Linux/Mac):
   ```bash
   # Standard-Pfad: ~/.zigbee2mqtt/data/devices/custom/
   mkdir -p ~/.zigbee2mqtt/data/devices/custom/
   cp gas-o-meter2.yaml ~/.zigbee2mqtt/data/devices/custom/
   ```

   #### Home Assistant Add-on:
   ```bash
   # Pfad: /config/zigbee2mqtt/data/devices/custom/
   # Über SSH Add-on oder Samba:
   cp gas-o-meter2.yaml /config/zigbee2mqtt/data/devices/custom/
   ```

### Schritt 3: Zigbee2MQTT neu starten

Zigbee2MQTT muss neu gestartet werden, damit die neue Device-Definition geladen wird:

#### Docker:
```bash
docker restart zigbee2mqtt
# Oder über Docker Compose:
docker-compose restart zigbee2mqtt
```

#### Native Installation:
```bash
# Systemd Service
sudo systemctl restart zigbee2mqtt

# Oder manuell gestartet:
# Prozess beenden (Ctrl+C) und neu starten
```

#### Home Assistant Add-on:
- Öffne Home Assistant → **Einstellungen** → **Add-ons**
- Wähle **Zigbee2MQTT** aus
- Klicke auf **Neu starten**

### Schritt 4: Installation prüfen

Nach dem Neustart prüfe die Logs, ob die Device-Definition erfolgreich geladen wurde:

**Logs anzeigen:**

#### Docker:
```bash
docker logs zigbee2mqtt | grep -i "gas-o-meter2"
# Oder alle Logs:
docker logs zigbee2mqtt
```

#### Native:
```bash
# Systemd Journal
journalctl -u zigbee2mqtt -f

# Oder Log-Datei (falls konfiguriert)
tail -f ~/.zigbee2mqtt/log/zigbee2mqtt.log
```

#### Home Assistant Add-on:
- In der Zigbee2MQTT Add-on Seite → **Logs** Tab

**Erfolgreiche Installation zeigt:**

**Für External Converter (.mjs):**
```
Loaded external converter: gas-o-meter2
```

**Für YAML Definition (.yaml):**
```
Loaded device definition: gas-o-meter2
```

**Falls Fehler auftreten:**
- Prüfe Dateisyntax (JavaScript/YAML)
- Stelle sicher, dass die Datei im richtigen Verzeichnis liegt
- Prüfe Dateiberechtigungen (Zigbee2MQTT muss Datei lesen können)
- Prüfe, ob der `data_path` in der `configuration.yaml` korrekt ist

### Schritt 5 (Optional): Icon hinzufügen

**Methode 1: Base64-kodiertes Icon (empfohlen, eingebettet):**
- Erstelle ein Icon mit folgenden Spezifikationen:
  - **Format:** PNG
  - **Größe:** 512x512 Pixel
  - **Hintergrund:** Transparent (Alpha-Kanal)
  - **Farbtiefe:** 
    - **24-bit (RGB)** oder **32-bit (RGBA)** empfohlen für beste Qualität
    - **8-bit (256 Farben)** möglich, wenn Icon wenige Farben benötigt
    - → Kleinere Dateigröße = kürzerer Base64-String = bessere Lesbarkeit
- Konvertiere zu Base64:
  ```bash
  # Linux/Mac
  base64 -i gas-o-meter2.png > icon_base64.txt
  
  # Windows (PowerShell)
  [Convert]::ToBase64String([IO.File]::ReadAllBytes("gas-o-meter2.png")) | Out-File icon_base64.txt
  
  # Oder Online-Tool verwenden: https://www.base64-image.de/
  ```
- Formatiere als Data URI: `data:image/png;base64,<base64-string>`
- Füge in der Device-Definition im `meta:` Bereich ein:
  - **Für .mjs:** `icon: 'data:image/png;base64,...'` im `meta` Objekt
  - **Für .yaml:** `icon: 'data:image/png;base64,...'` im `meta:` Bereich

**Methode 2: Externes Icon (Alternative):**
- Erstelle ein Icon mit den gleichen Spezifikationen wie Methode 1 (siehe oben)
- Speichere als `gas-o-meter2.png` in:
  ```
  <zigbee2mqtt-data-path>/device_icons/gas-o-meter2.png
  ```
- Referenziere in der Device-Definition:
  - **Für .mjs:** `icon: 'device_icons/gas-o-meter2.png'` im `meta` Objekt
  - **Für .yaml:** `icon: 'device_icons/gas-o-meter2.png'` im `meta:` Bereich

## Voraussetzungen

- ESP32C6 muss `model_id = "Gas-O-Meter2"` im ZigBee-Stack konfiguriert haben
- ESP32C6 muss `manufacturer_name = "Custom"` im ZigBee-Stack konfiguriert haben
- Siehe `src/transfer_zigbee.cpp` für die entsprechende Konfiguration
- Die Basic Cluster Attribute (Model ID und Manufacturer Name) werden automatisch beim Stack-Start gesetzt

## Verwendung

### Device Pairing

1. **ESP32C6 in Pairing-Modus versetzen:**
   - Über Web-UI: "ZigBee Pairing starten" Button
   - Oder über Serial-Konsole (falls konfiguriert)

2. **In Zigbee2MQTT Pairing aktivieren:**
   - Zigbee2MQTT Web-UI öffnen
   - Obere Leiste: **Permit join** aktivieren (oder Button klicken)
   - ESP32C6 sollte sich innerhalb von 60 Sekunden verbinden

3. **Device erscheint in Zigbee2MQTT:**
   - Name: `gas-o-meter2` (oder konfigurierter Friendly Name)
   - Model: `gas-o-meter2`
   - Vendor: `Custom`

### Verfügbare Datenpunkte

Nach erfolgreichem Pairing stehen folgende Datenpunkte zur Verfügung:

- **`gas_counter`** - Gas-Zählerstand in m³ (Simple Metering Cluster)
  - Typ: Numeric
  - Einheit: m³
  - Bereich: 0 - 999999.99
  - Cluster: Simple Metering (0x0702)
  - Attribute: currentSummationDelivered

- **`battery`** - Akku-Ladezustand in Prozent
  - Typ: Numeric
  - Einheit: %
  - Bereich: 0 - 100
  - Cluster: Power Configuration (0x0001)
  - Attribute: batteryPercentageRemaining

- **`firmware_version`** - Firmware-Version des ESP32C6
  - Typ: Text/String
  - Format: z.B. "1.0.0" oder "v1.2.3"
  - Cluster: Basic (0x0000)
  - Attribute: appVersion

### Reporting-Konfiguration

Das Device konfiguriert automatisch beim Pairing:

- **Battery Reporting:**
  - Minimum Report Interval: 3600 Sekunden (1 Stunde)
  - Maximum Report Interval: 86400 Sekunden (24 Stunden)
  - Reportable Change: 1%

- **Gas Counter Reporting:**
  - Minimum Report Interval: 300 Sekunden (5 Minuten)
  - Maximum Report Interval: 3600 Sekunden (1 Stunde)
  - Reportable Change: 0 (jede Änderung wird gemeldet - zeitbasierte Übertragung)

### Home Assistant Integration

Wenn Zigbee2MQTT mit Home Assistant verbunden ist, wird das Device automatisch als Entity erkannt:
- `sensor.gas_o_meter2_gas_counter` - Gas-Zählerstand
- `sensor.gas_o_meter2_battery` - Akku-Ladezustand
- `sensor.gas_o_meter2_firmware_version` - Firmware-Version

## Anpassungen

Die Device-Definition kann angepasst werden für:
- Zusätzliche Exposes (z.B. Temperatur, etc.)
- Custom Cluster-Definitionen
- Reporting-Intervalle
- Home Assistant Integration

**Für External Converter (.mjs):**
- Siehe [Zigbee2MQTT External Converters Guide](https://www.zigbee2mqtt.io/advanced/more/external_converters.html) für Details
- Nutze moderne Extends aus `zigbee-herdsman-converters/lib/modernExtend`

**Für YAML Definition (.yaml):**
- Siehe [Zigbee2MQTT Device Definition Guide](https://www.zigbee2mqtt.io/advanced/zigbee/01_how_to_create_support_for_new_devices.html) für Details

## Troubleshooting

### Device wird nicht erkannt

1. **Prüfe Model ID und Manufacturer Name:**
   - ESP32C6 muss `model_id = "Gas-O-Meter2"` senden
   - ESP32C6 muss `manufacturer_name = "Custom"` senden
   - Prüfe die Logs beim Pairing, ob diese Werte korrekt sind

2. **Prüfe Dateipfad:**
   - External Converter: Muss in `<data_path>/external_converters/` liegen (IM `data_path`, neben `configuration.yaml`)
   - YAML Definition: Muss in `<data_path>/devices/custom/` liegen (IM `data_path`)
   - Prüfe, wo die `configuration.yaml` liegt → das Verzeichnis ist der `data_path`

3. **Prüfe Dateinamen:**
   - External Converter: Muss `.mjs` Endung haben
   - YAML Definition: Muss `.yaml` Endung haben

4. **Prüfe Syntax:**
   - JavaScript: Prüfe auf Syntax-Fehler (z.B. fehlende Kommas, Klammern)
   - YAML: Prüfe auf YAML-Syntax-Fehler (z.B. Einrückung, Doppelpunkte)

### Device wird erkannt, aber Datenpunkte fehlen

1. **Prüfe Cluster-Konfiguration:**
   - Basic Cluster (0x0000) muss vorhanden sein
   - Power Configuration Cluster (0x0001) muss vorhanden sein
   - Simple Metering Cluster (0x0702) muss vorhanden sein

2. **Prüfe Attribute:**
   - Model ID (0x0005) muss im Basic Cluster gesetzt sein
   - Manufacturer Name (0x0004) muss im Basic Cluster gesetzt sein
   - Battery Percentage (0x0021) muss im Power Configuration Cluster vorhanden sein
   - Current Summation Delivered (0x0000) muss im Simple Metering Cluster vorhanden sein

3. **Prüfe Reporting:**
   - Reporting muss beim Pairing konfiguriert werden
   - Prüfe die Logs, ob Reporting erfolgreich war

## Weitere Informationen

- [Zigbee2MQTT External Converters](https://www.zigbee2mqtt.io/advanced/more/external_converters.html)
- [Zigbee2MQTT Device Definition Guide](https://www.zigbee2mqtt.io/advanced/zigbee/01_how_to_create_support_for_new_devices.html)
- [Zigbee2MQTT Modern Extends API](https://github.com/Koenkk/zigbee-herdsman-converters/blob/master/lib/modernExtend.ts)
