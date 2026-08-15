<!-- translation-source: README.md -->
<!-- translation-source-blob: 63e9f091575163aae0fedb2da878e210d35877fc -->

# Gas-O-Meter2 Web Flasher

[Wechsel zu Deutsch](README.md)

The GitHub Pages site installs Gas-O-Meter2 firmware with
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) via Web Serial.
PlatformIO is not required for end users.

## Browser and connection

- Use Firefox 151+ on desktop, Chrome, or Edge.
- On first connect, Firefox prompts to install an automatically generated,
  site-specific permission extension. No extra third-party extension or native
  helper app is required.
- The page must be served over HTTPS.
- Connect the XIAO ESP32-C6 with a USB data cable.
- If no port appears: check cable, USB drivers, and boot mode.

## Selection

| Mode | Use | Flash offset |
| ---- | --- | ------------ |
| Complete | First install or a release marked **Breaking**; resets persistent data | `0x0` |
| Firmware | Update program code only | `0x10000` |
| LittleFS | Update web UI and default config; overwrites current config | `0x285000` |

For a normal update, firmware and LittleFS can be flashed separately one after
the other. `pulse_nv` and the Zigbee partitions stay unchanged.

**For firmware and LittleFS partial updates, never choose “Erase device” in the
ESP Web Tools dialog.** Without Improv Serial, ESP Web Tools may erase the
entire flash even though the manifest only contains a partial image. Partial
manifests therefore force a confirmation prompt and disable the Improv wait.

A PCB Complete image fills gaps with `0xFF` and thereby resets NVS, the counter
ring buffer, Zigbee data, and LittleFS. TPL_test Complete ends after the test
firmware and contains no LittleFS.

`TPL_test` is a hardware test for TPL5110 and the reed contact. It has no
LittleFS, replaces production firmware, and must afterward be replaced again
with the matching PCB firmware.

## Deployment

Versioned HTML/CSS/JS files live in this folder. Firmware and manifests are not
checked in; the release workflow stages them under `pages_site/firmware/`.

Configure once in the repository:

1. GitHub → **Settings** → **Pages**
2. Set **Build and deployment / Source** to **GitHub Actions**

Workflow `.github/workflows/release.yml` deploys on each release tag `v*` when
it is the highest version tag. Manual rebuilds do not change Pages, so an older
tag cannot reset `latest`. A manual run without a tag is a smoke build only and
changes neither the release nor Pages.

Full maintainer instructions: [`RELEASING.md`](../RELEASING.md).

## License notices

The web flasher shows the project license, related source, and third-party
notices via `legal.html`. The release workflow copies `LICENSE`, `NOTICE`, and
`THIRD_PARTY_NOTICES.md` into the Pages build.
