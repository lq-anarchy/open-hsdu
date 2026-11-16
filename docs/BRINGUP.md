# Bring-up: first power-on to a whole game

Nothing in this repository has run on hardware yet (v0.1.0). These steps are in the order that finds each kind of
problem as cheaply as possible, and each one says what a pass looks like. **Log every step** (a template is at the
end). Only the packs under test are switched on.

**Before anything transmits:** the attenuator and antenna must both be fitted (or a proper dummy load), the build
must be the default one (power capped at the lowest steps), and you must have read [REGULATORY.md](REGULATORY.md).

## 0. Bench

- Wire the board as in [HARDWARE.md](HARDWARE.md); check each wire against the silkscreen. **First check the board:**
  the ESP32-S3-Zero has one RGB LED on GPIO21. Some other "ESP32-S3 Mini" boards put it on another pin, and a board
  whose LED is on any of GPIO8 to 13 conflicts with the wiring: look for a single row of pads on the underside and read
  the schematic.
- Software-defined radio receiver (an RTL-SDR is enough), a few feet away. Fix its gain and do not change it between
  comparisons.
- Python 3 with `pyserial`: `pip install pyserial`.

## 1. The board and the chip talk

```
(cd firmware && pio run -t upload)          # hold BOOT while plugging in, EVERY time (see below)
python3 tools/openhsdu_bench.py find        # all commands here are run from the repository root
python3 tools/openhsdu_bench.py console <port> selftest
```

**Uploading:** the firmware turns off the software route into the ROM loader (`enableReboot(false)`), so every upload
needs BOOT held while plugging the cable in; upload; then unplug and replug (or press RESET) *without* BOOT.

**Pass:** `part=0x00 version=0x14 detected`, `registers: all match profile proven`, `patable: ok`,
`marcstate=0x0D (receiving)`, `PASS`.

| If it says | Look at |
|---|---|
| no serial port ever appears | the image must be for 4 MB flash (`python3 tools/check_firmware_image.py firmware/.pio/build/esp32s3zero`); reflash with BOOT held |
| `NOT A CC1101`, part/version `0xFF` or `0x00` for both | wiring of CSN, MISO, MOSI, SCK; 3.3 V; module orientation |
| `version=` something else but `part=0x00` | a clone or newer silicon: the firmware refuses it; see `Cc1101::detect` |
| `registers: N differ` | a loose SPI wire (intermittent); try `regs` and look for a pattern |
| `not receiving` | the chip did not change state: check VCC and that the crystal runs |
| `radio=MISSING` in `status` | the boot failed; the modem is disabled and cannot transmit. Fix the cause and reboot: the UART's transmit pin is connected only after a successful boot, so `profile` cannot retry it |

Then **check the SmartRF registers**: generate the register set for 418.0 MHz, 4.8 kBaud, 2-FSK, 47.6 kHz deviation,
270 kHz bandwidth, asynchronous serial in TI's SmartRF Studio and compare `FSCAL3..0` and `TEST2..0` with
`core/src/profiles.cpp` (they are the TI recommended values for 433 MHz; calibration results are expected to differ).

## 2. The signal is where it should be (no pack involved)

Tune the SDR to 418.0 MHz, 1 to 2 MS/s.

1. `carrier 500` on the console. **Pass:** a single clean line at about 418.048 MHz (carrier + deviation: mark is
   the upper tone), 1 kHz-class width, for about 0.6 s (the 500 ms you asked for, which includes the 85 ms lead-in, plus
   the 80 ms tail). If it is near 417.95 MHz the data pin idles low: check GDO0 and that the UART is idle-high.
2. `pattern 32`. **Pass:** two tone lines about 85 to 95 kHz apart, about 418.048 and 417.952 MHz (proven profile).
3. `profile matched`, repeat 1 and 2. **Pass:** lines about 418.053 and 417.971 MHz (within about 2 kHz of the HSDU's
   own 418.054 and 417.969). `profile proven` to go back.
4. If a working HSDU is available, send one of its frames and compare level and spectrum with the new unit at the
   same distance and gain.
5. **Measure the radiated level once**, at 3 m, with a calibrated receiver or at a pre-compliance lab. The SDR alone
   gives only relative levels, and an SDR's antenna jack may carry DC bias (some dongles put 4.5 V on it).
   Compare with the limit in [REGULATORY.md](REGULATORY.md). Do not proceed to transmit near packs if it is over.

## 3. Does RTS reach the firmware? (the Windows question)

```
python3 tools/openhsdu_bench.py rts <port>
```

It switches the one port between console and data, sets key mode `rts`, sends a short burst with RTS low and then high,
and puts the key mode back to whatever it found (the firmware's default is `rts`). **Pass:** `RTS reached the firmware and keyed the transmitter` and `with RTS low ... nothing was
transmitted`. If RTS never arrives, set the key mode to `either` (or `data`) on the console, `keymode either`: it is
stored in flash, and the firmware then keys on the bytes and supplies the lead-in and tail itself. (The cost: any
program that opens the port and writes will then transmit.) (This step transmits a few bytes: attenuator and antenna fitted.)

## 4. Echo

```
python3 tools/openhsdu_bench.py echo <port>
```

**Pass:** the transmitted bytes come back both with RTS and without it. This is also what lets a host program find the
radio by itself.

## 5. One pack, close

One pack switched on, within a metre. Run your host program and send a poll.

**Pass:** the pack acknowledges (the SDR sees its reply and the host program decodes it). Try the pack whose data link has
the narrowest deviation first: the receiver is set for ±47.6 kHz and that one is furthest from it. If it does not
answer: try `profile matched`; check the SDR sees the base's tones where step 2 said; look at `status` counters
(`faults`, `watchdog`, `dropped`). Also note whether the *first* reply after a long idle is worse than the rest (the
offset compensator can drift without a carrier; THEORY 3.2).

## 6. Uplink margin and range walk

The 20 dB attenuator also costs 20 dB on receive. With a pack answering at 1 m, read `rssi` on the console several times
and record it, then repeat at 2, 4 and 8 m. Compare with the sensitivity: if the pack's reply is already near the noise
floor at a few metres the uplink, not the downlink, sets the range (THEORY section 7).

Then the range walk: same pack, a person wearing it as in play, 2 m steps, repeated polls at each distance. Record where
replies start to fail and compare with the table in THEORY section 7 (3.4 to 7.6 m free space at the default power step,
about 1 to 2.4 m with body loss). This decides whether one base covers the arena.

## 7. A whole game, then every pack

Run a short game with the packs that power up. Compare the frames decoded by the host with a second receiver (the
SDR) to find losses.

## Log template

| Date | Step | Packs on | Profile / power | Result (numbers) | Notes |
|---|---|---|---|---|---|
| | | | | | |
