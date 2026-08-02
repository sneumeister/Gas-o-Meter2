#!/usr/bin/env python3
"""Tests for release partition-layout validation."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from release_layout import (
    LOW_FREE_WARNING_BYTES,
    ReleaseLayoutError,
    validate_firmware_image,
)


PARTITIONS = """\
# Name, Type, SubType, Offset, Size, Flags
nvs, data, nvs, 0x9000, 0x7000,
factory, app, factory, 0x10000, 0x30000,
storage, data, littlefs, 0x40000, 64K,
"""


class ReleaseLayoutTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        self.partitions = self.root / "partitions.csv"
        self.partitions.write_text(PARTITIONS, encoding="utf-8")
        self.firmware = self.root / "firmware.bin"

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_accepts_image_at_warning_threshold(self) -> None:
        self.firmware.write_bytes(b"\0" * 0x2E000)

        fit = validate_firmware_image(self.firmware, self.partitions)

        self.assertEqual(fit.image_size, 0x2E000)
        self.assertEqual(fit.partition_size, 0x30000)
        self.assertEqual(fit.free_bytes, LOW_FREE_WARNING_BYTES)

    def test_accepts_image_below_warning_threshold(self) -> None:
        self.firmware.write_bytes(b"\0" * 0x2F000)

        fit = validate_firmware_image(self.firmware, self.partitions)

        self.assertEqual(fit.free_bytes, 0x1000)
        self.assertLess(fit.free_bytes, LOW_FREE_WARNING_BYTES)

    def test_rejects_image_larger_than_partition(self) -> None:
        self.firmware.write_bytes(b"\0" * 0x30001)

        with self.assertRaisesRegex(
            ReleaseLayoutError,
            "1 Bytes größer",
        ):
            validate_firmware_image(self.firmware, self.partitions)


if __name__ == "__main__":
    unittest.main()
