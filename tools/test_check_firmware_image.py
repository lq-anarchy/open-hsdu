"""Tests for check_firmware_image.py using synthetic images. Run: python3 -m pytest tools"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))

import check_firmware_image as chk  # noqa: E402


def image(size_nibble, length=1000):
    return bytes([0xE9, 2, 2, (size_nibble << 4) | 0x0F]) + bytes(length - 4)


def entry(label, ptype, subtype, offset, size):
    return b"\xaa\x50" + struct.pack("<BBII", ptype, subtype, offset, size) + label.encode().ljust(16, b"\0") + bytes(4)


def build(tmp_path, nibble=2, boot_nibble=2, parts=None, fw_len=1000):
    parts = parts or [("nvs", 1, 2, 0x9000, 0x5000), ("app0", 0, 0x10, 0x10000, 0x140000)]
    (tmp_path / "firmware.bin").write_bytes(image(nibble, fw_len))
    (tmp_path / "bootloader.bin").write_bytes(image(boot_nibble))
    (tmp_path / "partitions.bin").write_bytes(b"".join(entry(*p) for p in parts) + b"\xeb\xeb" + bytes(30))
    return str(tmp_path)


def test_a_4mb_build_passes(tmp_path):
    assert chk.check(build(tmp_path)) == []


def test_an_8mb_image_header_fails(tmp_path):
    problems = chk.check(build(tmp_path, nibble=3))
    assert any("firmware.bin says 8 MB" in p for p in problems)


def test_an_8mb_bootloader_fails(tmp_path):
    assert any("bootloader.bin says 8 MB" in p for p in chk.check(build(tmp_path, boot_nibble=3)))


def test_a_partition_beyond_4mb_fails(tmp_path):
    parts = [("app0", 0, 0x10, 0x10000, 0x140000), ("spiffs", 1, 0x82, 0x300000, 0x200000)]
    assert any("beyond 4 MB" in p for p in chk.check(build(tmp_path, parts=parts)))


def test_a_partition_that_just_fits_passes(tmp_path):
    parts = [("app0", 0, 0x10, 0x10000, 0x140000), ("spiffs", 1, 0x82, 0x300000, 0x100000)]
    assert chk.check(build(tmp_path, parts=parts)) == []


def test_a_firmware_larger_than_its_partition_fails(tmp_path):
    parts = [("app0", 0, 0x10, 0x10000, 500)]
    assert any("larger than the app partition" in p for p in chk.check(build(tmp_path, parts=parts, fw_len=1000)))


def test_missing_files_are_reported(tmp_path):
    assert any("missing" in p for p in chk.check(str(tmp_path)))


def test_a_file_that_is_not_an_image_is_rejected(tmp_path):
    d = build(tmp_path)
    (tmp_path / "firmware.bin").write_bytes(b"not an image")
    try:
        chk.check(d)
        raised = False
    except ValueError:
        raised = True
    assert raised
