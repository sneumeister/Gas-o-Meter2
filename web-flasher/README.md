[↓ Switch to English](README_EN.md)

# Gas-O-Meter2 Web-Flasher

Die GitHub-Pages-Seite installiert Gas-O-Meter2-Firmware mit
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) per Web Serial.
PlatformIO ist für Endnutzer nicht erforderlich.

Die Installationsseite selbst ist zweisprachig: deutsch unter `/`, englisch unter
[`index_en.html`](./index_en.html).

## Browser und Verbindung

- Firefox ab Version 151 auf Desktop-Systemen, Chrome oder Edge verwenden.
- Firefox fordert beim ersten Verbinden zur Installation einer automatisch
  erzeugten, seitenspezifischen Berechtigungs-Erweiterung auf. Eine zusätzliche
  Drittanbieter-Erweiterung oder native Hilfsanwendung ist nicht erforderlich.
- Die Seite muss über HTTPS aufgerufen werden.
- XIAO ESP32-C6 über ein USB-Datenkabel verbinden.
- Falls kein Port erscheint: Kabel, USB-Treiber und Boot-Modus prüfen.

## Auswahl

| Modus | Verwendung | Flash-Offset |
| ----- | ---------- | ------------ |
| Complete | Erstinstallation oder als **Breaking** markiertes Release; setzt persistente Daten zurück | `0x0` |
| Firmware | Nur Programmcode aktualisieren | `0x10000` |
| LittleFS | Web-UI und Default-Konfiguration aktualisieren; überschreibt die aktuelle Config | `0x285000` |

Bei einem normalen Update können Firmware und LittleFS einzeln und
nacheinander geflasht werden. `pulse_nv` und die Zigbee-Partitionen bleiben
dabei unverändert.

**Bei Firmware- und LittleFS-Teilupdates im ESP-Web-Tools-Dialog niemals
„Erase device“ wählen.** Ohne Improv Serial kann ESP Web Tools sonst den
gesamten Flash löschen, obwohl das Manifest nur ein Teil-Image enthält. Die
Teil-Manifeste erzwingen deshalb eine Rückfrage und deaktivieren die
Improv-Wartezeit.

Ein PCB-Complete-Image enthält mit `0xFF` gefüllte Zwischenbereiche und setzt
dadurch NVS, Zähler-Ringspeicher, Zigbee-Daten und LittleFS zurück.
TPL_test Complete endet dagegen nach der Test-Firmware und enthält kein
LittleFS.

`TPL_test` ist ein Hardwaretest für TPL5110 und Reed-Kontakt. Er enthält kein
LittleFS, ersetzt die Produktiv-Firmware und muss danach wieder durch die
passende PCB-Firmware ersetzt werden.

## Bereitstellung

Die versionierten HTML-/CSS-/JS-Dateien liegen in diesem Ordner. Firmware und
Manifeste werden nicht eingecheckt, sondern vom Release-Workflow in
`pages_site/firmware/` bereitgestellt.

Einmalig im Repository konfigurieren:

1. GitHub → **Settings** → **Pages**
2. **Build and deployment / Source** auf **GitHub Actions** setzen

Der Workflow `.github/workflows/release.yml` deployed bei jedem Release-Tag
`v*`, sofern es der aktuell höchste Versionstag ist. Manuelle Rebuilds ändern
Pages nicht, damit ein alter Tag `latest` nicht zurücksetzt. Ein manueller Lauf
ohne Tag ist nur ein Smoke-Build und verändert weder Release noch Pages.

Die vollständige Maintainer-Anleitung steht in
[`RELEASING.md`](../RELEASING.md).

## Lizenzhinweise

Der Web-Flasher zeigt über `legal.html` die Projektlizenz, den zugehörigen
Quellcode und Drittanbieterhinweise an. Der Release-Workflow kopiert dafür
`LICENSE`, `NOTICE` und `THIRD_PARTY_NOTICES.md` in den Pages-Build.
