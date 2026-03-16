# Node-RED BLE Integration – Gas-O-Meter2

Node-RED Flow Templates für BLE-Datenübertragung vom Gas-O-Meter2.

---

## 1. Voraussetzungen (Linux/Debian)

### Pakete installieren

```bash
sudo apt-get install bluetooth bluez libbluetooth-dev libudev-dev
```

### bluetoothd prüfen

`bluetoothd` **muss laufen** – `node-red-contrib-generic-ble` nutzt die BlueZ D-Bus API.

```bash
sudo systemctl status bluetooth
sudo systemctl enable bluetooth
```

### Berechtigungen

Node-RED-User muss in der Gruppe `bluetooth` sein:

```bash
sudo usermod -aG bluetooth <node-red-user>
```

Danach neu einloggen oder Node-RED neu starten.

### BLE-Adapter prüfen

```bash
bluetoothctl
# In bluetoothctl:
power on
scan on
```

BLE-Geräte sollten erscheinen. Mit `scan off` und `exit` beenden.

**Wichtig:** `hcitool lescan` funktioniert **nicht** zuverlässig mit vielen BLE-Dongles (z.B. CSR 0a12:0001). `bluetoothctl` nutzt den aktuellen BlueZ-Stack und ist der empfohlene Test.

### Proxmox VM (USB-Passthrough)

Falls Node-RED in einer Proxmox-VM läuft, muss der USB-BLE-Dongle durchgereicht werden:

```bash
# Auf dem Proxmox-Host:
qm set <VM-ID> -usb0 host=0a12:0001
```

Prüfen in der VM: `lsusb` sollte den Dongle zeigen.

---

## 2. Node-RED Paket installieren

**Paket:** `node-red-contrib-generic-ble` (BlueZ D-Bus API)

**Interaktiv (empfohlen):**

1. Node-RED UI → Menü (☰) → Manage palette → Install
2. Suche "generic ble" → `node-red-contrib-generic-ble` → Install
3. Node-RED neu starten

**Kommandozeile:**

```bash
cd ~/.node-red
npm install node-red-contrib-generic-ble
```

Node-RED neu starten.

**npm-Warnungen:** Deprecation-Warnings von transitiven Abhängigkeiten (inflight, npmlog, rimraf etc.) sind normal und blockieren die Installation nicht.

---

## 3. Ersteinrichtung (BLE-Pairing)

Einmaliger Setup-Prozess, um Node-RED mit dem Gas-O-Meter2 zu verbinden. Es handelt sich **nicht** um echtes BLE-Pairing (kein Schlüsselaustausch, keine Verschlüsselung) – lediglich die MAC-Adresse wird im Config-Node gespeichert.

### Schritt-für-Schritt

**ESP32C6-Seite:**

1. Web-Frontend des Gas-O-Meter2 öffnen (config.html)
2. Transfer-Modus auf **"BLE"** setzen
3. Button **"BLE Pairing starten"** klicken
4. ESP32C6 startet BLE-Advertising (90 Sekunden Timeout)

**Node-RED-Seite:**
5. Generic BLE Config-Node öffnen (Stift-Icon im BLE-Node)
6. **"BLE Scanning"** aktivieren → Gas-O-Meter2 erscheint im Dropdown
7. Gerät auswählen → **Apply** → MAC-Adresse wird gespeichert
8. **Done** → **Deploy**

### Danach

- Advertising endet automatisch, Einrichtung ist abgeschlossen.
- Bei jedem Wake-up verbindet sich Node-RED automatisch mit der gespeicherten MAC.
- **Wiederholung** nur nötig bei neuem ESP32C6 oder geänderter MAC.

**Wichtig:** **„BLE Scanning“ muss im Generic BLE Config dauerhaft aktiviert bleiben.** Nur so kann Node-RED das Gerät bei jedem Wake-up finden und die Verbindung aufbauen. Ist „BLE Scanning“ deaktiviert, zeigt der Node zwar ggf. „disconnected“ (wenn das Gerät von woanders gesehen wird), baut aber keine Verbindung auf – der ESP32 läuft dann 90 s ins Advertising-Timeout („Keine BLE-Verbindung in 90 s“).

---

## 4. Flow installieren

1. Node-RED UI → Menü (☰) → Import
2. `gas-o-meter2-ble-flow.json` auswählen
3. **Generic BLE Config** öffnen → BLE Scanning aktivieren → Gas-O-Meter2 wählen → Apply
4. MQTT-Broker konfigurieren (falls verwendet)
5. Deploy

Alle BLE-Nodes („BLE Notify (0xFFF1)“, „BLE Read (0x2A26)“, „BLE Zeit schreiben (0x2A2B)“) verwenden **dieselbe** Generic-BLE-Config (gleiche MAC).

**Zeitablauf am ESP32:** 90 s Warten auf Verbindung → nach Connect 3 s Verzögerung (`BLE_NOTIFY_DELAY_MS`), dann ein Notify auf 0xFFF1 → Session bis 180 s. Der Zeitsync (Write 0x2A2B) erfolgt nach dem Notify innerhalb dieser Session.

**Test ohne Hardware:** Der Inject-Node „BLE Mock (Test)“ sendet Mock-Daten direkt in die Pipeline (Parse BLE Data → MQTT/Debug) – ohne ESP32.

---

## 5. Flow-Übersicht und Funktionen

### Ablauf (vereinfacht)

1. **Beim Deploy:** „BLE Scanning starten“ (einmal) startet das Scanning.
2. **Alle 2 s:** „BLE Notify Trigger“ schickt je nach Status entweder **Connect** oder **Subscribe 0xFFF1** an „BLE Notify (0xFFF1)“ (siehe BLE-Status-Handling).
3. **Bei Notify:** „BLE Notify“ liefert die Messdaten → **On Notify (0xFFF1)** speichert die Nachricht und löst einen Read 0x2A26 aus → **BLE Read (0x2A26)** führt den Read aus → **On Read Result (0x2A26)** hängt die Firmware an und schickt an Parse BLE Data, Raw BLE und CTS Write.
4. **Parse BLE Data** erzeugt das einheitliche Payload (gas, battery, …) → MQTT, Dashboard, optional HA Discovery.

### BLE-Status-Handling (weniger Debug-Meldungen)

Die Library `node-red-contrib-generic-ble` wirft **„Not yet connected“**, wenn ein **Subscribe** ausgelöst wird, obwohl noch keine Verbindung besteht. Um diese Meldungen zu reduzieren, macht der Flow Folgendes:

- **„Nur bei sichtbarem Gerät triggern“** (alle 2 s):
  - Status **missing** → es wird nichts gesendet.
  - Status **disconnected** oder **connecting** → es wird nur **Connect** an den BLE-Node geschickt (kein Subscribe → kein „Not yet connected“).
  - Status **connected** → es wird **Subscribe 0xFFF1** geschickt; dann ist die Verbindung bereits da, der Subscribe gelingt.

Ein **Status-Node** liest den aktuellen Status von „BLE Notify (0xFFF1)“ und speichert ihn in `flow.bleNotifyStatus`. Die Function entscheidet daran, ob Connect oder Subscribe gesendet wird. So entstehen deutlich weniger „Not yet connected“-Meldungen im Debug.

### Firmware-Abfrage (0x2A26)

Die Firmware-Version steht in der Characteristic **0x2A26** (Read only). Der Flow holt sie **nach** jedem Notify:

1. **On Notify (0xFFF1):** Erkennt Notify-Daten (0xFFF1), speichert die Nachricht in `flow.lastNotifyMsg` und gibt eine Nachricht `{ topic: '2a26' }` aus.
2. **BLE Read (0x2A26):** Erhält diese Nachricht, führt den GATT-Read 0x2A26 aus und gibt das Ergebnis aus.
3. **On Read Result (0x2A26):** Liest den Firmware-String aus dem Payload, holt die gespeicherte Notify-Nachricht, setzt `msg.firmware` und reicht die Nachricht an Parse BLE Data, Raw BLE und CTS Write weiter.

So erscheint in den geparsten Daten und in MQTT die echte Firmware-Version statt „unknown“.

### Home Assistant Auto-Discovery

- **HA Discovery AN / AUS:** Zwei Inject-Nodes schalten die Discovery ein oder aus (Payload `true` / `false`).
- **HA Discovery Config:** Baut die MQTT-Config-Nachrichten für die Entities (Gas, Battery, Spannung, Battery Low, Firmware) und publiziert sie (AN) bzw. löscht sie (AUS) per leerem Payload.
- **HA Discovery** (MQTT out): Sendet die Nachrichten an den konfigurierten MQTT-Broker; Topic z. B. `homeassistant/sensor/gas_o_meter2_gas/config` usw.

Nach „HA Discovery AN“ erscheinen die Entities in Home Assistant, sofern der MQTT-Broker mit HA verbunden ist. Die Daten kommen über das gleiche Topic wie die normalen MQTT-Daten (`gas-o-meter2/data`).

---

## 6. Gas-O-Meter2 BLE-Characteristics

| UUID   | Bedeutung        | Lese/Schreib |
|--------|------------------|--------------|
| 0xFFF1 | Messdaten (JSON) | Read/Notify  |
| 0x2A26 | Firmware         | Read         |
| 0x2A2B | Current Time     | Read/Write   |

---

## 7. SBFspot-Koexistenz

SBFspot (Classic Bluetooth, RFCOMM Kernel-Sockets) und `node-red-contrib-generic-ble` (BLE, BlueZ D-Bus) können **parallel** auf demselben Dual-Mode-Adapter laufen:

- SBFspot nutzt Classic BT über RFCOMM → Kernel-Ebene
- generic-ble nutzt BLE über BlueZ D-Bus → Userspace
- `bluetoothd` verwaltet beides → kein Konflikt
- SBFspot-Cron-Jobs laufen unabhängig von Node-RED

---

## 8. Troubleshooting

| Problem | Lösung |
| ------- | ------ |
| generic-ble findet keine Geräte | `bluetoothctl scan on` prüfen, ob BLE-Geräte sichtbar sind |
| "Permission denied" | User in Gruppe `bluetooth`? → `groups <user>` prüfen |
| bluetoothd läuft nicht | `sudo systemctl start bluetooth` |
| Dongle nicht sichtbar (Proxmox) | USB-Passthrough konfigurieren: `qm set <VM-ID> -usb0 host=...` |
| Deprecation-Warnings bei npm install | Normal (transitive Abhängigkeiten), blockiert nicht |
| **Node-RED stürzt ab:** `indexOf is not a function` in `PeripheralRemovableNoble.onMiss` | Siehe Abschnitt **8a** (Bug in node-red-contrib-generic-ble beim „device missed“). |

### 8a. Crash: `indexOf is not a function` (onMiss)

**Symptom:** Node-RED beendet sich mit  
`TypeError: this._discoveredPeripheralUUids.indexOf is not a function`  
in `node-red-contrib-generic-ble/dist/noble/index.js` (Zeile ~61), ausgelöst durch `onDeviceMissed` (BlueZ meldet Gerät als „missing“). Folge: Keine Nachricht bei „Raw BLE“, Status springt von „missing“ auf „disconnected“, Service startet immer wieder neu.

**Ursache:** Bug im Paket – bei „device missed“ wird `.indexOf` auf einer Variable aufgerufen, die in diesem Pfad kein Array ist.

**Workaround (manueller Patch im Paket):**

1. Datei öffnen (Pfad je nach Installation, z. B.):

   ```text
   ~/.node-red/node_modules/node-red-contrib-generic-ble/dist/noble/index.js
   ```

2. In der Funktion `onMiss` (ca. Zeile 61) die Zeile mit `this._discoveredPeripheralUUids.indexOf` finden.
3. **Davor** eine Absicherung einbauen, z. B. direkt zu Beginn der `onMiss`-Funktion:

   ```javascript
   if (!Array.isArray(this._discoveredPeripheralUUids)) {
       this._discoveredPeripheralUUids = [];
   }
   ```

4. Node-RED neu starten.

**Dauerhafte Lösung:** Issue beim Maintainer melden (GitHub: CANDY-LINE/node-red-contrib-generic-ble) oder auf ein Update des Pakets warten, das `_discoveredPeripheralUUids` in diesem Pfad als Array initialisiert bzw. prüft.

### 8b. „Disconnected“ im Node-RED, aber kein Debug-Output bei „Raw BLE“

**Symptom:** In `bluetoothctl` erscheint das Gerät (z. B. `[NEW] Device 98:A3:16:8F:D9:AA gas-o-meter2`), in Node-RED zeigt der BLE-Node „disconnected“, und der Debug-Node **„Raw BLE“** liefert keine Meldung.

**Erklärung:** Das ist in vielen Fällen **erwartetes Verhalten**:

- **„Raw BLE“** zeigt nur **empfangene Notify-Daten** (Payload auf Characteristic 0xFFF1). Er bekommt **keine** Nachrichten bei Verbindungsaufbau oder -abbau – nur wenn der ESP32 tatsächlich eine Notify-Nachricht sendet.
- **„disconnected“** ist der **Status** des Nodes (verbunden/getrennt). Wenn das Gerät nur kurz sichtbar ist und wieder verschwindet („device missed“), oder die Verbindung abbricht, **bevor** der ESP32 Daten sendet, bleibt der Status „disconnected“ und es kommt **keine** Nachricht an „Raw BLE“.

**Typische Szenarien:**

| Situation | bluetoothctl | Node-RED Status | Raw BLE |
| --------- | ------------- | --------------- | ------- |
| Gerät sendet Werte (Wake-up, Notify) | Gerät sichtbar, ggf. verbunden | connected → danach disconnected | **Ja** (einmal pro Notify) |
| Gerät nur sichtbar, sendet nicht | NEW Device, RSSI-Updates | disconnected | **Nein** |
| Verbindung bricht vor dem ersten Notify ab | – | disconnected | **Nein** |

**Was prüfen:**

1. **„Raw BLE“ aktivieren:** Im Flow den Debug-Node „Raw BLE“ mit Rechtsklick → **Enable** (oder Doppelklick → Häkchen bei „Enable“), dann Deploy. Ohne Aktivierung erscheint dort nichts.
2. **ESP32 sendet wirklich:** Gas-O-Meter2 sendet nur bei Wake-up (z. B. Tastendruck oder periodisch) einmalig über 0xFFF1. Erst dann sollte „Raw BLE“ eine Meldung bekommen.
3. **Timing:** Wenn der ESP32 sehr schnell wieder in den Schlaf geht oder die Verbindung vor dem ersten Notify abbricht, siehst du „disconnected“ ohne Raw-BLE-Output.

Kurz: **Raw BLE = nur empfangene Nutzdaten.** Status „disconnected“ ohne Raw-BLE-Ausgabe bedeutet in der Regel: Es ist (noch) keine Notify-Nachricht angekommen.

### 8c. „Missing → Disconnected → Missing“, aber keine Daten (kein Crash)

**Symptom:** Node-RED zeigt den Zyklus Missing → Disconnected → Missing, stürzt aber nicht mehr ab. Trotzdem kommen keine Werte bei „Raw BLE“ oder im Flow an.

**Ursache:** Der ESP32 hat das Notify **sofort** nach dem Connect gesendet. Der Central (Node-RED/generic-ble) braucht aber einige Sekunden für Service Discovery und das Abonnieren der Characteristic 0xFFF1. Wenn das Notify vor dem Subscription kommt, geht die Nachricht verloren.

**Lösung (ESP32-Firmware):** Ab einer bestimmten Firmware-Version wartet der ESP32 nach dem Connect **3 Sekunden** (`BLE_NOTIFY_DELAY_MS` in `include/ble_config.h`), bevor er das erste Notify sendet. Bitte die neueste Firmware flashen. Falls auf einem langsamen System (z. B. schwache VM) auch dann keine Daten ankommen, kann `BLE_NOTIFY_DELAY_MS` auf 5000 erhöht werden.

---

## Hinweise

- BLE wird erst aktiv, wenn der Transfer-Modus **BLE** auf dem ESP32C6 aktiviert ist.
- generic-ble nutzt BlueZ D-Bus – **bluetoothd** muss laufen (nicht stoppen).
