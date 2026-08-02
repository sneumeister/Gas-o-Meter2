#!/usr/bin/env python3
"""Validate Gas-O-Meter2 release artifacts and ESP Web Tools manifests."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from release_layout import (
    LOW_FREE_WARNING_BYTES,
    ReleaseLayoutError,
    validate_firmware_image,
)


PROJECT_ROOT = Path(__file__).resolve().parents[1]
PCB_ENVS = ("PCB_20251022", "PCB_20260523")
TPL_ENV = "TPL_test"
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="Version without leading v")
    parser.add_argument("--out", default="dist", help="Release output directory")
    parser.add_argument(
        "--project-root",
        help="Project source directory containing partitions.csv",
    )
    return parser.parse_args()


def expected_files(
    env_name: str, version: str
) -> dict[str, tuple[str, int, bool]]:
    prefix = f"gas-o-meter2_{env_name}_v{version}"
    manifests = {
        "manifest-complete.json": (f"{prefix}_complete.bin", 0, False),
        "manifest-firmware.json": (
            f"{prefix}_firmware.bin",
            0x10000,
            True,
        ),
    }
    if env_name in PCB_ENVS:
        manifests["manifest-littlefs.json"] = (
            f"{prefix}_littlefs.bin",
            0x285000,
            True,
        )
    return manifests


def validate_manifest(
    env_dir: Path,
    manifest_name: str,
    expected_binary: str,
    expected_offset: int,
    prompt_before_erase: bool,
    version: str,
) -> list[str]:
    errors: list[str] = []
    manifest_path = env_dir / manifest_name
    if not manifest_path.is_file():
        return [f"Fehlt: {manifest_path}"]

    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"Ungültiges JSON {manifest_path}: {exc}"]

    if manifest.get("version") != version:
        errors.append(
            f"{manifest_path}: version={manifest.get('version')!r}, erwartet {version!r}"
        )

    builds = manifest.get("builds")
    if not isinstance(builds, list) or len(builds) != 1:
        return errors + [f"{manifest_path}: genau ein Build erwartet"]

    build = builds[0]
    if build.get("chipFamily") != "ESP32-C6":
        errors.append(f"{manifest_path}: chipFamily muss ESP32-C6 sein")

    parts = build.get("parts")
    if not isinstance(parts, list) or len(parts) != 1:
        return errors + [f"{manifest_path}: genau ein Part erwartet"]

    part = parts[0]
    if part.get("path") != expected_binary:
        errors.append(
            f"{manifest_path}: path={part.get('path')!r}, erwartet {expected_binary!r}"
        )
    if part.get("offset") != expected_offset:
        errors.append(
            f"{manifest_path}: offset={part.get('offset')!r}, "
            f"erwartet {expected_offset}"
        )

    if prompt_before_erase:
        if manifest.get("new_install_prompt_erase") is not True:
            errors.append(
                f"{manifest_path}: new_install_prompt_erase muss true sein"
            )
        if manifest.get("new_install_improv_wait_time") != 0:
            errors.append(
                f"{manifest_path}: new_install_improv_wait_time muss 0 sein"
            )

    binary_path = env_dir / expected_binary
    if not binary_path.is_file() or binary_path.stat().st_size == 0:
        errors.append(f"Binärdatei fehlt oder ist leer: {binary_path}")

    return errors


def validate_environment(
    version_dir: Path,
    env_name: str,
    version: str,
    partitions_csv: Path,
) -> list[str]:
    errors: list[str] = []
    env_dir = version_dir / env_name
    if not env_dir.is_dir():
        return [f"Environment-Ordner fehlt: {env_dir}"]

    manifests = expected_files(env_name, version)
    expected_names = set(manifests)
    expected_names.update(
        binary_name for binary_name, _, _ in manifests.values()
    )
    actual_names = {path.name for path in env_dir.iterdir() if path.is_file()}

    missing = expected_names - actual_names
    unexpected = actual_names - expected_names
    if missing:
        errors.append(f"{env_dir}: fehlend: {', '.join(sorted(missing))}")
    if unexpected:
        errors.append(f"{env_dir}: unerwartet: {', '.join(sorted(unexpected))}")

    firmware_name = manifests["manifest-firmware.json"][0]
    try:
        fit = validate_firmware_image(env_dir / firmware_name, partitions_csv)
        if fit.free_bytes < LOW_FREE_WARNING_BYTES:
            print(
                f"WARNUNG: {env_dir / firmware_name} hat nur "
                f"{fit.free_bytes} Bytes Reserve; weniger als "
                f"{LOW_FREE_WARNING_BYTES} Bytes frei",
                file=sys.stderr,
            )
    except ReleaseLayoutError as exc:
        errors.append(f"{env_dir}: {exc}")

    for manifest_name, (
        binary_name,
        offset,
        prompt_before_erase,
    ) in manifests.items():
        errors.extend(
            validate_manifest(
                env_dir,
                manifest_name,
                binary_name,
                offset,
                prompt_before_erase,
                version,
            )
        )
    return errors


def main() -> int:
    args = parse_args()
    version = args.version.removeprefix("v")
    if not VERSION_RE.fullmatch(version):
        print(f"FEHLER: Ungültige Version: {args.version!r}", file=sys.stderr)
        return 1

    output_root = Path(args.out)
    if not output_root.is_absolute():
        output_root = PROJECT_ROOT / output_root
    version_dir = output_root / version
    project_root = (
        Path(args.project_root).resolve()
        if args.project_root
        else PROJECT_ROOT
    )
    partitions_csv = project_root / "partitions.csv"

    errors: list[str] = []
    for env_name in (*PCB_ENVS, TPL_ENV):
        errors.extend(
            validate_environment(
                version_dir,
                env_name,
                version,
                partitions_csv,
            )
        )

    forbidden = {"bootloader.bin", "partition-table.bin", "partitions.bin"}
    if version_dir.exists():
        for path in version_dir.rglob("*"):
            if path.is_file() and path.name in forbidden:
                errors.append(f"Nicht zu veröffentlichende Datei gefunden: {path}")

    if errors:
        print("Release-Verifikation fehlgeschlagen:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1

    print(f"Release-Artefakte gültig: {version_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
