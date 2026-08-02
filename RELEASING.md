# Neues Release erstellen

Diese Anleitung richtet sich an Maintainer. Der Release-Workflow erzeugt
Firmware-Binaries, GitHub-Release-Assets und den Web-Flasher automatisch.

## Voraussetzungen (einmalig)

1. GitHub → **Settings** → **Pages**
2. **Build and deployment / Source** auf **GitHub Actions** setzen
3. Prüfen, dass GitHub Actions Schreibrechte für Releases und Pages verwenden
   darf.

## 1. Release vorbereiten

Auf einem sauberen `main` arbeiten:

```bash
git switch main
git pull --ff-only
git status
```

In `include/version.h` muss `PROJECT_VERSION` auf die neue Version ohne
führendes `v` gesetzt werden, beispielsweise:

```cpp
const char PROJECT_VERSION[] = "1.0.2";
```

Versionserhöhung und Release-Änderungen committen und über den normalen
Review-/PR-Prozess nach `main` bringen. Vor dem Tag müssen diese drei Angaben
identisch sein:

- `PROJECT_VERSION`: `1.0.2`
- Git-Tag: `v1.0.2`
- gewünschter GitHub Release: `v1.0.2`

`dependencies.lock` ist absichtlich versioniert. Dadurch verwenden lokale
Builds und GitHub Actions dieselben ESP-IDF-Komponenten. Abhängigkeiten nur
bewusst aktualisieren, anschließend Lockfile und Firmwaregröße gemeinsam
prüfen. Auch die PIOArduino-Plattform ist in `platformio.ini` auf eine feste
Release-Version gepinnt.

## 2. Update-Art bestimmen

In PR-/Commit-Titeln und der Beschreibung klar angeben, was Nutzer flashen
sollen:

| Änderung | Anweisung |
| -------- | --------- |
| Partitionstabelle, Bootloader oder Flash-Layout geändert | `BREAKING: Complete flash required` |
| Nur Programmcode geändert | Firmware flashen |
| Nur Dateien unter `data/` geändert | LittleFS flashen |
| Code und `data/` geändert | Firmware und LittleFS nacheinander flashen |

Complete setzt persistente Konfiguration, NVS-Zähler-Ringspeicher und
Zigbee-Daten zurück. LittleFS überschreibt die aktuelle Gerätekonfiguration.
Diese Auswirkungen müssen in den Release Notes ausdrücklich stehen.
Bei Firmware-/LittleFS-Web-Updates muss zusätzlich darauf hingewiesen werden,
im ESP-Web-Tools-Dialog niemals **„Erase device“** zu wählen.

Der Workflow ergänzt automatisch generierte Release Notes. Bei einem
Breaking-Release muss deshalb mindestens ein relevanter PR-/Commit-Titel mit
`BREAKING:` beginnen und „Complete flash required“ enthalten.

## 3. Optionaler Smoke-Build

Vor dem Tag:

1. GitHub → **Actions** → **Release firmware and web flasher**
2. **Run workflow**
3. Feld `tag` leer lassen
4. Nach erfolgreichem Lauf das Actions-Artifact
   `gas-o-meter2-dist-<VERSION>` herunterladen und prüfen

Dieser Modus erstellt **kein** GitHub Release und deployed **keine** Pages.

Lokal ist dieselbe Prüfung möglich:

```bash
python scripts/make_release_bins.py --version 1.0.2
python scripts/verify_release_dist.py --version 1.0.2
```

Die Release-Skripte brechen ab, wenn ein Firmware-Image nicht in die
`factory`-Partition aus `partitions.csv` passt. Bei weniger als 8 KiB Reserve
geben sie eine deutliche Warnung aus, lassen den Release aber zu. Die harte
Größenprüfung darf für einen Release nicht umgangen werden.

## 4. Tag erstellen und Release auslösen

Erst taggen, wenn der freigegebene Stand auf `main` liegt:

```bash
git switch main
git pull --ff-only
git status
git log -1 --oneline
git tag -a v1.0.2 -m "Gas-O-Meter2 v1.0.2"
git push origin v1.0.2
```

Jeder Tag `v*` startet `.github/workflows/release.yml` automatisch.

## 5. Release kontrollieren

Nach dem Workflow prüfen:

1. GitHub Action vollständig grün
2. GitHub Release `vX.Y.Z` vorhanden
3. Pro PCB-Version vorhanden:
   - `*_complete.bin`
   - `*_firmware.bin`
   - `*_littlefs.bin`
4. Für `TPL_test` vorhanden:
   - `*_complete.bin`
   - `*_firmware.bin`
5. Web-Flasher unter
   <https://sneumeister.github.io/Gas-o-Meter2/> erreichbar
6. Geräte- und Installationsauswahl funktioniert
7. Manifest-URLs unter `firmware/latest/<ENV>/` liefern HTTP 200
8. Automatisch generierte Release Notes enthalten die korrekte
   Update-Empfehlung bzw. den Breaking-Hinweis

## Bestehenden Tag erneut bauen

1. GitHub → **Actions** → **Release firmware and web flasher**
2. **Run workflow**
3. Den vorhandenen Tag eingeben

Der Workflow baut den exakten Quellstand des Tags mit dem aktuellen
Release-Tooling und ersetzt gleichnamige Assets. Das nur verwenden, wenn der
Tag bereits alle heute erforderlichen Build-Environments enthält.
Manuelle Rebuilds deployen bewusst keine Pages und ändern deshalb
`firmware/latest` nicht.

**Wichtig für `v1.0.1`:** Dieser Tag enthält noch kein `TPL_test`-Environment.
Ein vollständiger Rebuild mit PCB- und TPL-Assets ist deshalb nicht möglich.
Nach Merge der Release-Infrastruktur `PROJECT_VERSION` erhöhen und einen neuen
Tag (empfohlen: `v1.0.2`) erstellen. Nicht neuere Quellen als `v1.0.1`
deklarieren und nicht still den bestehenden Tag verschieben.

## Fehlerfall

- Fehlgeschlagene Action zuerst korrigieren; keine unvollständigen Assets
  manuell als „fertig“ deklarieren.
- Einen veröffentlichten und bereits verwendeten Tag nicht verschieben.
- Nach einer Codekorrektur einen neuen Patch-Tag erstellen, beispielsweise
  `v1.0.3`.
- Nur wenn ein falscher Tag garantiert noch nicht verwendet wurde, darf er
  dokumentiert gelöscht und neu gesetzt werden.

Nützliche Kontrollen:

```bash
git status
git log -1 --oneline
git tag --list
```
