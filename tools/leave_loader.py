#!/usr/bin/env python3
"""Restart an ESP32-S3 that sits in its ROM flash loader back into the firmware, over USB, with nothing pressed.

usage: python3 tools/leave_loader.py PORT        (the loader's port: "USB JTAG/serial debug unit")

Why this exists: after an upload the board is still in the loader. A reset sent over USB (what upload tools do) is a
core reset: the chip keeps the boot-pin state it latched, so a loader entered with the BOOT button comes straight back
as the loader, and the usual advice is to unplug and replug. A reset by the chip's RTC watchdog is a system reset: the
pins are read again and the firmware starts. All four register writes go in ONE session, because opening the port a
second time can itself reset the chip and lose the unlock.

Needs esptool (pip install esptool, or PlatformIO's copy on PYTHONPATH). Tried with esptool 4.5.1 on Windows.
"""
import sys

RTC_BASE = 0x60008000            # ESP32-S3 RTC_CNTL
WDT_CONFIG0 = RTC_BASE + 0x98
WDT_CONFIG1 = RTC_BASE + 0x9C
WDT_WPROTECT = RTC_BASE + 0xB0
WDT_KEY = 0x50D83AA1


def leave_loader(esp):
    """`esp` is a connected esptool loader object (anything with write_reg)."""
    esp.write_reg(WDT_WPROTECT, WDT_KEY)                               # unlock the RTC watchdog
    esp.write_reg(WDT_CONFIG1, 2000)                                   # stage 0 timeout: a few tens of ms
    esp.write_reg(WDT_CONFIG0, (1 << 31) | (5 << 28) | (1 << 8) | 2)   # enable; stage 0 resets the system
    try:
        esp.write_reg(WDT_WPROTECT, 0)                                 # lock again (the chip may already be gone)
    except Exception:
        pass


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print(__doc__)
        return 2
    from esptool.targets import ESP32S3ROM
    esp = ESP32S3ROM(argv[0], 115200)
    esp.connect("no_reset")
    leave_loader(esp)
    print("watchdog armed: the board restarts into its firmware in a moment")
    return 0


if __name__ == "__main__":
    sys.exit(main())
