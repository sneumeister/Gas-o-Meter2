#!/usr/bin/env python3
"""Verify English Markdown translations against German source blob hashes."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

SOURCE_RE = re.compile(
    r"<!--\s*translation-source:\s*(?P<source>[^\s>]+)\s*-->"
)
BLOB_RE = re.compile(
    r"<!--\s*translation-source-blob:\s*(?P<blob>[0-9a-fA-F]+)\s*-->"
)

# English files relative to repository root.
TRANSLATION_FILES = (
    "README_EN.md",
    "README_WEBUI_EN.md",
    "web-flasher/README_EN.md",
    "integration_templates/README_EN.md",
    "integration_templates/nodered/README_EN.md",
    "integration_templates/nodered/PATCH-generic-ble-onMiss_EN.md",
    "integration_templates/zigbee2mqtt/README_EN.md",
    "KiCAD-PCB/README_EN.md",
    "KiCAD-PCB/Version20251022/README_EN.md",
    "KiCAD-PCB/Version20260523/README_EN.md",
    "CAD-housing/README_EN.md",
)


def git_hash_object(path: Path) -> str:
    result = subprocess.run(
        ["git", "hash-object", str(path)],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def check_translation(repo_root: Path, relative_en: str) -> list[str]:
    errors: list[str] = []
    en_path = repo_root / relative_en
    if not en_path.is_file():
        return [f"missing translation file: {relative_en}"]

    text = en_path.read_text(encoding="utf-8")
    source_match = SOURCE_RE.search(text)
    blob_match = BLOB_RE.search(text)
    if source_match is None:
        errors.append(f"{relative_en}: missing translation-source comment")
    if blob_match is None:
        errors.append(f"{relative_en}: missing translation-source-blob comment")
    if source_match is None or blob_match is None:
        return errors

    source_rel = source_match.group("source")
    recorded_blob = blob_match.group("blob").lower()
    source_path = (en_path.parent / source_rel).resolve()
    try:
        source_path.relative_to(repo_root.resolve())
    except ValueError:
        errors.append(
            f"{relative_en}: translation-source escapes repository: {source_rel}"
        )
        return errors

    if not source_path.is_file():
        errors.append(
            f"{relative_en}: translation-source not found: {source_rel}"
        )
        return errors

    actual_blob = git_hash_object(source_path).lower()
    if recorded_blob != actual_blob:
        errors.append(
            f"{relative_en}: stale translation for {source_rel} "
            f"(recorded {recorded_blob}, current {actual_blob})"
        )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check English doc translations against German source blobs."
    )
    parser.add_argument(
        "--project-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="Repository root (default: parent of scripts/)",
    )
    args = parser.parse_args()
    repo_root = args.project_root.resolve()

    errors: list[str] = []
    for relative_en in TRANSLATION_FILES:
        errors.extend(check_translation(repo_root, relative_en))

    if errors:
        print("Documentation translation check failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print(f"OK: {len(TRANSLATION_FILES)} English translations match source blobs.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
