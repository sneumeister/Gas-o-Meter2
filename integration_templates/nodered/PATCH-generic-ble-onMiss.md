[↓ Switch to English](PATCH-generic-ble-onMiss_EN.md)

# Patch: node-red-contrib-generic-ble – Crash „indexOf is not a function“

**Betroffene Datei (Pfad je nach Installation):**

```text
~/.node-red/node_modules/node-red-contrib-generic-ble/dist/noble/index.js
```

Unter Debian/System-Node-RED (User `nodered`):

```text
/home/nodered/.node-red/node_modules/node-red-contrib-generic-ble/dist/noble/index.js
```

*Hinweis:* In manchen Installationen lautet der Pfad anders. Bei Zweifel die Datei so finden:  
`find ~/.node-red -name "index.js" 2>/dev/null | xargs grep -l "_discoveredPeripheralUUids" 2>/dev/null`

---

## Ersetzung durch `index.js_patch` (generic-ble v4.0.3)

Die Original-`index.js` im Paket ist komprimiert/minifiziert und daher fehleranfällig beim manuellen Bearbeiten. **Bei node-red-contrib-generic-ble v4.0.3** kann die Datei stattdessen durch die bereits gepatchte, lesbar formatierte Version ersetzt werden:

- Im Projekt liegt unter `integration_templates/nodered/index.js_patch` eine lesbare, gepatchte Fassung (gleicher Inhalt wie die Original-`index.js`, nur formatiert und mit dem onMiss-Fix).
- Ziel-Pfad der zu ersetzenden Datei wie oben (z. B.  
  `~/.node-red/node_modules/node-red-contrib-generic-ble/dist/noble/index.js`).

**Vorgehen:**

1. Alte `index.js` sichern (z. B. `index.js.bak`).
2. `index.js_patch` aus dem gas-o-meter2-Repo nach `…/dist/noble/index.js` kopieren (Pfad anpassen).
3. Node-RED neu starten.

Damit entfällt die manuelle Snippet-Einfügung in die minifizierte Datei.

---

## Snippet (am Anfang der Funktion `onMiss` einfügen)

Die Funktion `onMiss` beginnt in etwa so (Zeile ~58–65):

```javascript
onMiss(peripheral) {
    // HIER EINFÜGEN (ganz am Anfang der Funktion):
    if (!Array.isArray(this._discoveredPeripheralUUids)) {
        this._discoveredPeripheralUUids = [];
    }
    // … restlicher bestehender Code (z. B. this._discoveredPeripheralUUids.indexOf(...)) …
}
```

**Nur diesen Block einfügen** (3 Zeilen), direkt nach der Zeile `onMiss(peripheral) {`:

```javascript
    if (!Array.isArray(this._discoveredPeripheralUUids)) {
        this._discoveredPeripheralUUids = [];
    }
```

---

## Anwendung

1. Datei mit einem Editor öffnen (z. B. `nano` oder `vim`).
2. Zeile mit `onMiss(peripheral)` bzw. `onMiss (peripheral)` suchen.
3. Gleich **darunter** (nach der öffnenden `{`) die drei Zeilen des Snippets einfügen.
4. Speichern und Node-RED neu starten (z. B. `sudo systemctl restart nodered`).

Nach dem Patch sollte der „device missed“-Pfad nicht mehr abstürzen; der Flow kann weiterlaufen und bei erneutem Connect/Notify Daten liefern.
