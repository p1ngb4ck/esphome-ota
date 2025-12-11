#!/usr/bin/env python3
"""
Extract OTA app partition binary from factory image for dual-partition OTA.

This script extracts the partition binary content (not full flash image) from
a compiled firmware, suitable for writing to the OTA helper partition.

Usage:
    python tools/extract_ota_app_partition.py ota-app/.esphome/build/ota-app/.pioenvs/ota-app/firmware.bin ota-app-partition.bin
"""

import sys
from pathlib import Path


def extract_partition_binary(firmware_bin: Path, output_bin: Path):
    """
    Extract partition binary from firmware.bin (removes bootloader offset).

    For ESP32, the firmware.bin at 0x10000 is the actual partition content.
    This is already the partition binary - just copy it.
    """
    if not firmware_bin.exists():
        print(f"Error: Input file not found: {firmware_bin}")
        sys.exit(1)

    # For ESP32, the .bin file in build output is already the partition content
    # (PlatformIO/ESP-IDF outputs app partition binary, not full flash image)
    with open(firmware_bin, "rb") as f_in:
        data = f_in.read()

    with open(output_bin, "wb") as f_out:
        f_out.write(data)

    print(f"Extracted {len(data)} bytes from {firmware_bin} to {output_bin}")
    print(f"\nTo flash this partition:")
    print(f"  esphome upload main.yaml --device /dev/ttyUSB0 \\")
    print(f"    --ota-helper-bin {output_bin} --ota-helper-offset 0xYOUR_OFFSET")
    print(f"\nTo find YOUR_OFFSET, check your partition table CSV file.")


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)

    firmware_bin = Path(sys.argv[1])
    output_bin = Path(sys.argv[2])

    extract_partition_binary(firmware_bin, output_bin)


if __name__ == "__main__":
    main()
