#!/usr/bin/env python3
"""Shared partition-layout checks for release tooling."""

from __future__ import annotations

import csv
from dataclasses import dataclass
from pathlib import Path


FACTORY_PARTITION = "factory"
EXPECTED_APP_OFFSET = 0x10000
LOW_FREE_WARNING_BYTES = 8 * 1024


class ReleaseLayoutError(RuntimeError):
    """Raised when an artifact is incompatible with the flash layout."""


@dataclass(frozen=True)
class Partition:
    name: str
    offset: int
    size: int


@dataclass(frozen=True)
class FirmwareFit:
    image_size: int
    partition_size: int
    free_bytes: int


def parse_size(value: str) -> int:
    """Parse ESP-IDF partition numbers such as 0x230000, 64K or 1M."""

    normalized = value.strip().lower()
    multiplier = 1
    if normalized.endswith("k"):
        normalized = normalized[:-1]
        multiplier = 1024
    elif normalized.endswith("m"):
        normalized = normalized[:-1]
        multiplier = 1024 * 1024
    try:
        return int(normalized, 0) * multiplier
    except ValueError as exc:
        raise ReleaseLayoutError(
            f"Ungültige Partitionsgröße oder Offset: {value!r}"
        ) from exc


def read_partition(partitions_csv: Path, name: str) -> Partition:
    """Read a named partition with explicit offset and size."""

    if not partitions_csv.is_file():
        raise ReleaseLayoutError(f"Partitionstabelle fehlt: {partitions_csv}")

    with partitions_csv.open(encoding="utf-8", newline="") as handle:
        for row in csv.reader(handle):
            if not row:
                continue
            fields = [field.split("#", 1)[0].strip() for field in row]
            if not fields[0] or fields[0].startswith("#") or fields[0] != name:
                continue
            if len(fields) < 5 or not fields[3] or not fields[4]:
                raise ReleaseLayoutError(
                    f"Partition {name!r} benötigt expliziten Offset und Größe"
                )
            return Partition(
                name=name,
                offset=parse_size(fields[3]),
                size=parse_size(fields[4]),
            )

    raise ReleaseLayoutError(
        f"Partition {name!r} fehlt in {partitions_csv}"
    )


def validate_firmware_image(
    firmware: Path,
    partitions_csv: Path,
) -> FirmwareFit:
    """Ensure a firmware image fits the factory partition."""

    if not firmware.is_file():
        raise ReleaseLayoutError(f"Firmware fehlt: {firmware}")

    partition = read_partition(partitions_csv, FACTORY_PARTITION)
    if partition.offset != EXPECTED_APP_OFFSET:
        raise ReleaseLayoutError(
            f"Factory-Offset ist 0x{partition.offset:x}, "
            f"erwartet 0x{EXPECTED_APP_OFFSET:x}"
        )

    image_size = firmware.stat().st_size
    free_bytes = partition.size - image_size
    if free_bytes < 0:
        raise ReleaseLayoutError(
            f"{firmware.name} ist {-free_bytes} Bytes größer als die "
            f"Factory-Partition ({image_size} > {partition.size})"
        )

    return FirmwareFit(
        image_size=image_size,
        partition_size=partition.size,
        free_bytes=free_bytes,
    )
