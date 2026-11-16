#!/usr/bin/env python3
"""Checks a built firmware against the 4 MB flash of the ESP32-S3FH4R2 (Waveshare ESP32-S3-Zero).

The devkit board definition PlatformIO uses assumes 8 MB. An image and partition table built for 8 MB do not boot on a
4 MB chip, and the failure is silent: no serial port ever appears. This reads the build outputs and fails if:
  * the flash-size field in the header of firmware.bin or bootloader.bin is not 4 MB, or
  * any partition in partitions.bin ends beyond 4 MB, or
  * the app partition is smaller than firmware.bin.

  python3 tools/check_firmware_image.py firmware/.pio/build/esp32s3zero
"""
import struct
import sys
from pathlib import Path

FLASH_BYTES = 4 * 1024 * 1024
SIZE_NIBBLE = {0x0: 1, 0x1: 2, 0x2: 4, 0x3: 8, 0x4: 16}  # MB, from the ESP image header


def image_flash_mb(path):
    data = Path(path).read_bytes()
    if len(data) < 4 or data[0] != 0xE9:
        raise ValueError("%s is not an ESP32 image (bad magic)" % path)
    return SIZE_NIBBLE.get(data[3] >> 4)


def partitions(path):
    data = Path(path).read_bytes()
    out = []
    for i in range(0, len(data) - 31, 32):
        entry = data[i:i + 32]
        if entry[0:2] == b"\xaa\x50":
            ptype, subtype, offset, size = struct.unpack("<BBII", entry[2:12])
            label = entry[12:28].split(b"\0")[0].decode("ascii", "replace")
            out.append((label, ptype, subtype, offset, size))
        elif entry[0:2] == b"\xeb\xeb":
            continue  # MD5 checksum entry
        else:
            break
    return out


def check(build_dir):
    build = Path(build_dir)
    problems = []
    for name in ("firmware.bin", "bootloader.bin"):
        p = build / name
        if not p.exists():
            problems.append("%s is missing" % p)
            continue
        mb = image_flash_mb(p)
        if mb != 4:
            problems.append("%s says %s MB of flash, the chip has 4 MB" % (name, mb))
    table = build / "partitions.bin"
    app_size = None
    if not table.exists():
        problems.append("%s is missing" % table)
    else:
        parts = partitions(table)
        if not parts:
            problems.append("partitions.bin has no partitions")
        for label, ptype, subtype, offset, size in parts:
            if offset + size > FLASH_BYTES:
                problems.append("partition %s ends at 0x%X, beyond 4 MB" % (label, offset + size))
            if ptype == 0 and subtype == 0x10 and app_size is None:  # first OTA/factory app slot
                app_size = size
            if ptype == 0 and subtype == 0x00 and app_size is None:
                app_size = size
    fw = build / "firmware.bin"
    if fw.exists() and app_size is not None and fw.stat().st_size > app_size:
        problems.append("firmware.bin (%d bytes) is larger than the app partition (%d bytes)" % (fw.stat().st_size, app_size))
    return problems


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    problems = check(argv[1])
    if problems:
        for p in problems:
            print("FAIL  " + p)
        return 1
    print("ok: images and partition table fit the 4 MB flash")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
