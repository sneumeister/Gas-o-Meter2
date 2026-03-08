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

---

## 4. Flow installieren

1. Node-RED UI → Menü (☰) → Import
2. `gas-o-meter2-ble-flow.json` auswählen
3. **Generic BLE Config** öffnen → BLE Scanning aktivieren → Gas-O-Meter2 wählen → Apply
4. MQTT-Broker konfigurieren (falls verwendet)
5. Deploy

**Test ohne Hardware:** Im Flow ist ein "BLE Mock (Test)"-Inject-Node enthalten. Dieser sendet Mock-Daten direkt an den Data Parser – damit lässt sich die gesamte Pipeline (Parser → MQTT → Debug) ohne ESP32C6 testen.

---

## 5. Gas-O-Meter2 BLE-Characteristics

| UUID   | Bedeutung        | Lese/Schreib |
|--------|------------------|--------------|
| 0xFFF1 | Messdaten (JSON) | Read/Notify  |
| 0x2A26 | Firmware         | Read         |
| 0x2A2B | Current Time     | Read/Write   |

---

## 6. SBFspot-Koexistenz

SBFspot (Classic Bluetooth, RFCOMM Kernel-Sockets) und `node-red-contrib-generic-ble` (BLE, BlueZ D-Bus) können **parallel** auf demselben Dual-Mode-Adapter laufen:

- SBFspot nutzt Classic BT über RFCOMM → Kernel-Ebene
- generic-ble nutzt BLE über BlueZ D-Bus → Userspace
- `bluetoothd` verwaltet beides → kein Konflikt
- SBFspot-Cron-Jobs laufen unabhängig von Node-RED

---

## 7. Troubleshooting

| Problem | Lösung |
|---------|--------|
| generic-ble findet keine Geräte | `bluetoothctl scan on` prüfen, ob BLE-Geräte sichtbar sind |
| "Permission denied" | User in Gruppe `bluetooth`? → `groups <user>` prüfen |
| bluetoothd läuft nicht | `sudo systemctl start bluetooth` |
| Dongle nicht sichtbar (Proxmox) | USB-Passthrough konfigurieren: `qm set <VM-ID> -usb0 host=...` |
| Deprecation-Warnings bei npm install | Normal (transitive Abhängigkeiten), blockiert nicht |

---

## Hinweise

- BLE wird erst aktiv, wenn Transfer-Mode "ble" auf dem ESP32C6 aktiviert ist
- generic-ble nutzt BlueZ D-Bus – `bluetoothd` muss laufen (nicht stoppen!)
- Siehe `docs/BLE_IMPLEMENTATION_PLAN.md` für den vollständigen Implementierungsplan
