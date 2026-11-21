# Hardware

## Parts (one unit)

| Part | Notes |
|---|---|
| ESP32-S3 board with a USB-C connector and pre-soldered header | Developed against the Waveshare ESP32-S3-Zero (ESP32-S3FH4R2: **4 MB flash**, 2 MB PSRAM; Waveshare also sells N8R8 variants with 8 MB, which run the same 4 MB image). Any ESP32-S3 board that exposes GPIO8–13 and uses USB-OTG works, but **check the board's own schematic**: its RGB LED is on GPIO21 on the Zero, and on any other board it must not be on a pin used here (GPIO8–13). The ESP32-C3 cannot be used: its only USB port resets the chip on RTS |
| CC1101 radio module, 8-pin 2.54 mm header, SMA antenna jack | Band at least 400–464 MHz. **No power amplifier** (modules sold as "20 dBm" have one, and are usually tuned for 433 MHz only: wrong for this). 3.3 V only. Many cheap modules have a matching network tuned for 433 MHz; 418 MHz is inside the chip's range but performance there is **UNPROVEN** (BRINGUP step 2) |
| 20 dB SMA attenuator, male to female, rated for the power | goes between the module's SMA jack and the antenna; see [REGULATORY.md](REGULATORY.md). **It attenuates what the base receives by the same 20 dB** (THEORY section 7). Its SMA connector does not by itself satisfy 15.203 (REGULATORY.md) |
| 433 MHz antenna with an SMA plug, **2 dBi or less** | e.g. a 2 dBi rubber duck; do not use a higher-gain antenna: the power budget assumes ≤ 2 dBi. Check the antenna's stated gain, not the seller's headline |
| 8 female-to-female jumper wires | no soldering with a pre-soldered board |
| USB-C data cable | a charge-only cable does not work |

Build two. One can be tested against the other before a pack is involved.

## Wiring

Module pin names vary between makers: wire by the **silkscreen name**, not the pin position.

| CC1101 module | ESP32-S3 | Notes |
|---|---|---|
| VCC | 3V3 | 3.3 V only; never 5 V |
| GND | GND | |
| CSN | GPIO10 | |
| MOSI (SI) | GPIO11 | |
| SCK | GPIO12 | |
| MISO (SO) | GPIO13 | |
| GDO0 | GPIO8 | the UART's transmit pin: data into the chip when transmitting |
| GDO2 | GPIO9 | the UART's receive pin: demodulated data out of the chip |

Avoid GPIO 0, 3, 45, 46 (boot straps), 19 and 20 (USB) and 21 (the on-board LED). `firmware/src/pins.h` holds these
as constants and `test/test_pins.cpp` checks them. If your board differs, change `pins.h` and nothing else.

The firmware leaves GPIO8 unconnected until the chip's own GDO0 pin has been set to high impedance (at power-on the chip
drives GDO0), so a reset does not put two outputs on one wire. A 330 Ω to 1 kΩ resistor in series with GDO0 is a cheap
extra guard if you want one.

Keep the wires short (under 15 cm) and the antenna away from the board and from the USB cable.

## Build and flash

```
cd firmware
pio run                       # compile (for 4 MB flash: see below)
pio run -t upload             # first hold BOOT while plugging the cable in, or send `loader now` (below)
```

The firmware disables the DTR/RTS route into the ROM loader so a stray line pattern cannot reboot the board. Two ways in
remain:

- **`loader now` on the console** (115200 baud): the board disconnects, then comes back as the ROM loader, a different
  serial device ("USB JTAG/serial debug unit"), about two seconds later. Upload to that port. Nothing is pressed.
- **BOOT held while plugging in**: always works, also with no firmware or a broken one.

Either way the board is still in the loader after the upload. `python3 tools/leave_loader.py PORT` restarts it into the
firmware over USB (it arms the chip's RTC watchdog; an ordinary reset over USB lands in the loader again when BOOT was
used). Unplugging and replugging without BOOT does the same.

Tried on unit 1 from Windows (2026-11-20), all without touching the board: `loader now`, upload with
`esptool.py --before no_reset --after no_reset write_flash 0x10000 firmware.bin`, `leave_loader.py`, self-test PASS.

Two environments: `esp32s3zero` (default; power capped at the low steps) and `esp32s3zero_highpower` (allows every
PATABLE step; **for a dummy load or cabled bench set-up only, never an antenna**).

**Flash size.** PlatformIO's generic ESP32-S3 board assumes 8 MB; the Zero has 4 MB. `platformio.ini` sets the 4 MB
flash size and partition table, and `python3 tools/check_firmware_image.py firmware/.pio/build/esp32s3zero` fails if
an image or partition would not fit. An image built for 8 MB does not boot on the 4 MB chip, and the symptom is that
no serial port ever appears. CI runs the check on every build.

After flashing, the board shows up as one serial port named `open-hsdu`. Open it at 115200 baud and run `selftest`.

## If the ESP32 resets while transmitting

The CC1101 has its own supply and keeps whatever state it was in: it will keep transmitting until the next boot resets
it, about half a second later. The loop watchdog (1 s) is a backstop, not a guarantee; see THEORY section 5.
