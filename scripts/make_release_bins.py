#!/usr/bin/env python3
"""Build versioned Gas-O-Meter2 release binaries and web-flash manifests."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator, Sequence

from release_layout import LOW_FREE_WARNING_BYTES, validate_firmware_image


PROJECT_ROOT = Path(__file__).resolve().parents[1]
BUILD_ROOT = PROJECT_ROOT / ".pio" / "build"
PCB_ENVS = ("PCB_20251022", "PCB_20260523")
TPL_ENV = "TPL_test"

APP_OFFSET = 0x10000
PARTITIONS_OFFSET = 0x8000
LITTLEFS_OFFSET = 0x285000

VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$")
HEADER_VERSION_RE = re.compile(
    r'PROJECT_VERSION\[\]\s*=\s*"(?P<version>[^"]+)"'
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--version",
        help="Release version without leading v (for example 1.0.1)",
    )
    parser.add_argument(
        "--out",
        default="dist",
        help="Output directory relative to the project root (default: dist)",
    )
    parser.add_argument(
        "--project-root",
        help="Project source directory (default: parent of this script)",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Reuse existing .pio/build artifacts",
    )
    return parser.parse_args()


def normalize_version(value: str) -> str:
    version = value.strip()
    if version.startswith("v"):
        version = version[1:]
    if not VERSION_RE.fullmatch(version):
        raise ValueError(f"Ungültige Release-Version: {value!r}")
    return version


def read_project_version() -> str:
    version_header = PROJECT_ROOT / "include" / "version.h"
    match = HEADER_VERSION_RE.search(version_header.read_text(encoding="utf-8"))
    if not match:
        raise RuntimeError(f"PROJECT_VERSION fehlt in {version_header}")
    return normalize_version(match.group("version"))


def resolve_version(explicit: str | None) -> str:
    project_version = read_project_version()
    selected_version: str | None = None

    if explicit:
        selected_version = normalize_version(explicit)
    else:
        github_ref = os.environ.get("GITHUB_REF_NAME", "")
        if github_ref.startswith("v"):
            selected_version = normalize_version(github_ref)

    if selected_version is None:
        return project_version
    if selected_version != project_version:
        raise RuntimeError(
            "Versionskonflikt: Release "
            f"{selected_version} != PROJECT_VERSION {project_version}"
        )
    return selected_version


def resolve_command(*names: str) -> str:
    for name in names:
        command = shutil.which(name)
        if command:
            return command
    raise RuntimeError(
        f"Keines der benötigten Programme ist im PATH: {', '.join(names)}"
    )


def run(command: Sequence[str]) -> None:
    print("+", subprocess.list2cmdline(list(command)), flush=True)
    subprocess.run(command, cwd=PROJECT_ROOT, check=True)


@contextmanager
def release_config() -> Iterator[None]:
    """Use the public example config for LittleFS and restore local state."""

    config = PROJECT_ROOT / "data" / "config.json"
    example = PROJECT_ROOT / "data" / "config.json_example"
    backup: bytes | None = config.read_bytes() if config.exists() else None

    if not example.is_file():
        raise FileNotFoundError(f"Release-Konfiguration fehlt: {example}")

    shutil.copy2(example, config)
    try:
        yield
    finally:
        if backup is None:
            config.unlink(missing_ok=True)
        else:
            config.write_bytes(backup)


def require_artifact(build_dir: Path, names: Sequence[str]) -> Path:
    for name in names:
        candidate = build_dir / name
        if candidate.is_file() and candidate.stat().st_size > 0:
            return candidate
    searched = ", ".join(str(build_dir / name) for name in names)
    raise FileNotFoundError(f"Build-Artefakt fehlt (gesucht: {searched})")


def find_common_artifacts(env_name: str) -> tuple[Path, Path, Path]:
    build_dir = BUILD_ROOT / env_name
    firmware = require_artifact(build_dir, ("firmware.bin",))
    partitions = require_artifact(
        build_dir,
        ("partitions.bin", "partition_table/partition-table.bin"),
    )
    bootloader = require_artifact(
        build_dir,
        ("bootloader.bin", "bootloader/bootloader.bin"),
    )
    return bootloader, partitions, firmware


def require_firmware_fit(firmware: Path) -> None:
    fit = validate_firmware_image(
        firmware,
        PROJECT_ROOT / "partitions.csv",
    )
    message = (
        f"{firmware.name}: {fit.image_size} Bytes, "
        f"{fit.free_bytes} Bytes Reserve"
    )
    if fit.free_bytes < LOW_FREE_WARNING_BYTES:
        print(
            f"WARNUNG: {message}; weniger als "
            f"{LOW_FREE_WARNING_BYTES} Bytes frei",
            file=sys.stderr,
            flush=True,
        )
    else:
        print(f"Firmwaregröße OK: {message}", flush=True)


def write_manifest(
    output_dir: Path,
    manifest_name: str,
    product_name: str,
    version: str,
    binary_name: str,
    offset: int,
    prompt_before_erase: bool = False,
) -> None:
    manifest = {
        "name": product_name,
        "version": version,
        "builds": [
            {
                "chipFamily": "ESP32-C6",
                "parts": [{"path": binary_name, "offset": offset}],
            }
        ],
    }
    if prompt_before_erase:
        # Firmware-only and LittleFS-only installs must never silently trigger
        # a full-chip erase when no Improv Serial device is detected.
        manifest["new_install_prompt_erase"] = True
        manifest["new_install_improv_wait_time"] = 0

    target = output_dir / manifest_name
    target.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def merge_binary(
    esptool: str,
    output: Path,
    bootloader: Path,
    partitions: Path,
    firmware: Path,
    littlefs: Path | None = None,
) -> None:
    command = [
        esptool,
        "--chip",
        "esp32c6",
        "merge-bin",
        "-o",
        str(output),
        "0x0",
        str(bootloader),
        hex(PARTITIONS_OFFSET),
        str(partitions),
        hex(APP_OFFSET),
        str(firmware),
    ]
    if littlefs is not None:
        command.extend([hex(LITTLEFS_OFFSET), str(littlefs)])
    run(command)


def build_pcb_release(
    env_name: str,
    version: str,
    version_dir: Path,
    esptool: str,
) -> None:
    build_dir = BUILD_ROOT / env_name
    bootloader, partitions, firmware = find_common_artifacts(env_name)
    require_firmware_fit(firmware)
    littlefs = require_artifact(build_dir, ("littlefs.bin",))

    output_dir = version_dir / env_name
    output_dir.mkdir(parents=True, exist_ok=True)
    prefix = f"gas-o-meter2_{env_name}_v{version}"

    firmware_name = f"{prefix}_firmware.bin"
    littlefs_name = f"{prefix}_littlefs.bin"
    complete_name = f"{prefix}_complete.bin"

    shutil.copy2(firmware, output_dir / firmware_name)
    shutil.copy2(littlefs, output_dir / littlefs_name)
    merge_binary(
        esptool,
        output_dir / complete_name,
        bootloader,
        partitions,
        firmware,
        littlefs,
    )

    product_name = f"Gas-O-Meter2 {env_name}"
    write_manifest(
        output_dir,
        "manifest-complete.json",
        product_name,
        version,
        complete_name,
        0,
    )
    write_manifest(
        output_dir,
        "manifest-firmware.json",
        product_name,
        version,
        firmware_name,
        APP_OFFSET,
        prompt_before_erase=True,
    )
    write_manifest(
        output_dir,
        "manifest-littlefs.json",
        product_name,
        version,
        littlefs_name,
        LITTLEFS_OFFSET,
        prompt_before_erase=True,
    )


def build_tpl_release(version: str, version_dir: Path, esptool: str) -> None:
    bootloader, partitions, firmware = find_common_artifacts(TPL_ENV)
    require_firmware_fit(firmware)
    output_dir = version_dir / TPL_ENV
    output_dir.mkdir(parents=True, exist_ok=True)
    prefix = f"gas-o-meter2_{TPL_ENV}_v{version}"

    firmware_name = f"{prefix}_firmware.bin"
    complete_name = f"{prefix}_complete.bin"
    shutil.copy2(firmware, output_dir / firmware_name)
    merge_binary(
        esptool,
        output_dir / complete_name,
        bootloader,
        partitions,
        firmware,
    )

    product_name = "Gas-O-Meter2 TPL5110 Hardwaretest"
    write_manifest(
        output_dir,
        "manifest-complete.json",
        product_name,
        version,
        complete_name,
        0,
    )
    write_manifest(
        output_dir,
        "manifest-firmware.json",
        product_name,
        version,
        firmware_name,
        APP_OFFSET,
        prompt_before_erase=True,
    )


def main() -> int:
    global PROJECT_ROOT, BUILD_ROOT

    args = parse_args()
    try:
        if args.project_root:
            PROJECT_ROOT = Path(args.project_root).resolve()
            BUILD_ROOT = PROJECT_ROOT / ".pio" / "build"
        if not (PROJECT_ROOT / "platformio.ini").is_file():
            raise FileNotFoundError(
                f"Kein PlatformIO-Projektverzeichnis: {PROJECT_ROOT}"
            )

        version = resolve_version(args.version)
        output_root = Path(args.out)
        if not output_root.is_absolute():
            output_root = PROJECT_ROOT / output_root
        version_dir = output_root / version

        if version_dir.exists():
            shutil.rmtree(version_dir)

        if not args.skip_build:
            pio = resolve_command("pio", "platformio")
            with release_config():
                for env_name in PCB_ENVS:
                    run([pio, "run", "-e", env_name])
                    run([pio, "run", "-e", env_name, "-t", "buildfs"])
                run([pio, "run", "-e", TPL_ENV])

        esptool = resolve_command("esptool", "esptool.py")
        for env_name in PCB_ENVS:
            build_pcb_release(env_name, version, version_dir, esptool)
        build_tpl_release(version, version_dir, esptool)

        print(f"Release-Artefakte erstellt: {version_dir}")
        return 0
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"FEHLER: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
