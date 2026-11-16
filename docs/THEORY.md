# Theory of operation

How the link works, why this design talks to it, and what each number in the code comes from. Read this before
changing a register value or a timer.

**Tags.** **LIVE** = measured on real equipment (software-defined-radio recordings and serial-port logs).
**DATASHEET** = from the Texas Instruments CC1101 datasheet (SWRS061) or its design notes. **CALC** = computed from
those, and the test that checks it is named. **DESIGN** = a choice this project made. **UNPROVEN** = not yet seen
working.

**Status (v0.1.0): the firmware has not yet run on hardware.** The radio settings are derived from a configuration
that real packs acknowledged (through a different board); everything else is tested in software against a model of
the chip and compiles for the ESP32-S3. [Section 9](#9-what-is-proven-and-what-is-not) lists exactly what is and is
not proven.

---

## 1. What an HSDU is

The HSDU is the base station of a laser-tag system: the one radio the game computer uses to talk to every pack.
Measured on a working unit, its job is only this:

```
  PC ──RS-232, 4800 baud, 8N1──►[ HSDU ]~~~~ one shared channel near 418 MHz ~~~~►  packs
      RTS = "transmit now"       2-FSK transmitter + receiver, and an echo
  PC ◄──────────────────────────[ HSDU ]◄~~~~ replies, one pack at a time ~~~~~~
```

- **It is a modem with a push-to-talk line.** While the PC holds RTS, bytes written to the serial port are sent as
  FSK. Whenever it is not transmitting, whatever the receiver demodulates comes out of the serial port. LIVE.
- **It is protocol-blind.** Addressing, framing, checksums and the game itself live in the PC program and the packs.
  LIVE: a pack's reply arrives with its own run of `0xAA` bytes in front of the frame, passed straight through.
- **It echoes.** Everything it transmits also comes back out of the serial port, which the PC program can use to find
  the radio and to tell its own frames from replies. LIVE.
- **It has no squelch.** With nothing on the air its receiver still delivers random bytes, so the PC filters them. LIVE.

That is the whole specification this project has to meet. *Replacing an HSDU means reproducing its radio signal and
those three behaviours, nothing else.* That is why the modem in this repository is deliberately small and why it
describes no game protocol: it contains none.

## 2. The air link

### 2.1 Modulation and bit timing (LIVE)

| Property | Value |
|---|---|
| Modulation | 2-FSK: two tones, one for each bit value |
| Bit rate | 4,800 baud, asynchronous: 1 start bit, 8 data bits LSB first, 1 stop bit, no parity |
| Bit period | 208.3 µs; one byte = 10 bits = 2.083 ms |
| Polarity | **mark (1) = upper tone**, space (0) = lower tone. A UART idles at 1, so an idle line is a steady carrier on the *upper* tone |
| Base station tones | upper 418.054 MHz, lower 417.969 MHz: centre about 418.011 MHz, deviation about ±42.5 kHz (shift about 85 kHz) |
| Tone purity | with the key down on one tone the carrier is a clean line, about 1 kHz wide at −3 dB |

Because a UART's idle level is mark, **polarity needs no inverter anywhere**: the chip's data pin idles high,
which is the upper tone, which is what the packs expect.

### 2.2 The packs transmit on slightly different tones (LIVE)

Each pack answers with its own small transmitter, and they are not identical. Across three measured data links the
centre frequency moved 41 kHz (417.979 to 418.020 MHz) and the deviation ran from ±26.5 to ±51 kHz. All are 2-FSK,
4,800 baud 8N1, mark = upper tone.

Consequence for the receiver: it must (a) have a channel filter wide enough for the widest sender plus the offset,
and (b) follow each sender's centre frequency. The chip does (b) with automatic frequency-offset compensation, and
the profile turns it on (section 3.2).

### 2.3 Anatomy of one transmission (LIVE)

```
 carrier up      9 × 0xAA      frame bytes ...            tail
 (mark tone)     preamble                                 (mark tone)
 |<-- ~85 ms -->|<- 18.8 ms ->|<--- n × 2.083 ms --->|<--- ~80 ms --->|
 ^ PTT on                                                     PTT off ^
```

- **Lead, about 85 ms.** The carrier was up on the mark tone about 85 ms before the first byte. In the system measured
  this lead came from the host program holding RTS before it wrote, not from the radio itself. **Whoever keys the
  radio must supply it.** This firmware supplies it itself when it is keyed by data rather than by RTS (section 4.2).
- **Preamble: nine `0xAA` bytes** (about 18.8 ms), as seen on the air. `0xAA` sent LSB first with start and stop bits is
  the pattern `0 0 1 0 1 0 1 0 1 1`: eight transitions in ten bits, a dense pattern a receiver's data slicer can settle
  on (DESIGN reading; the modem does not depend on it, it passes the bytes).
- **Tail, about 80 ms.** The carrier stayed up after the last byte (again the host's RTS hold in the system measured).
- **A pack replies after a delay and begins with its own carrier and preamble.** Measured request-to-reply gaps
  ran from about 280 ms (a live poll) to over a second (a find). During a live poll the pack's carrier is already up
  *while the base still holds its own*, so **a base must be listening again within about 180 ms of its last byte**.
  This design is back in receive about 3 to 4 ms after RTS drops, and within about 85 ms of the last byte when it
  supplies its own tail (tested in `test_integration.cpp` and `test_modem.cpp`).

### 2.4 One shared channel

The channel is half duplex and shared by every pack: the PC polls one pack at a time. A pack stuck transmitting
jams them all. A base never needs to listen while it transmits, which is why the modem discards received bytes during
transmit and for a short settle time after (section 4.3).

## 3. The CC1101 as the modem

The CC1101 is a single-chip sub-GHz transceiver (300–928 MHz). It is a good stand-in because (LIVE) a CC1101 running
a Flipper Zero's 418 MHz, 2-FSK, ±47.6 kHz, 4,798-baud preset has already transmitted frames that real packs
acknowledged and received their replies. **The register set in this repository is that preset plus five changes
(section 3.3) and has not yet run on hardware.**

### 3.1 Asynchronous serial mode

The chip normally frames data into packets with a sync word, a length and a CRC. For this link none of that is
wanted: the packs' own frames are inside the byte stream. **Asynchronous serial mode** (`PKTCTRL0.PKT_FORMAT = 3`)
turns the chip into a bare modulator and demodulator behind two pins:

| Pin | Direction (this design) | Meaning |
|---|---|---|
| GDO0 | into the chip while transmitting | the data to be sent as FSK: wired to the ESP32's UART transmit pin |
| GDO2 | out of the chip while receiving | the demodulated data: wired to the ESP32's UART receive pin |

Two register changes make the pins behave: `IOCFG0 = 0x2E` leaves GDO0 floating (high impedance) so the UART can
drive it, and `IOCFG2 = 0x0D` makes GDO2 output the demodulated serial data. The datasheet says the transmit modulator
samples the asynchronous data input at 8 times the programmed data rate, so an ordinary UART at 4,800 baud (bit-edge
error well under 12.5 % of a bit) is inside the sampling window. On the receive side the GDO2 output has about ±1/8-bit
jitter, which a UART's mid-bit sampling tolerates. **UNPROVEN with an ESP32 UART**; proven with a timer-driven source.

### 3.2 The chip's equations, and the numbers they give

Every value below is computed in `core/src/cc1101_math.cpp` and checked in `test/test_math.cpp` and
`test/test_profiles.cpp`. `f_xosc` is the 26 MHz crystal.

| Quantity | Equation (DATASHEET) | Register | Proven profile | HSDU-matched profile |
|---|---|---|---|---|
| Carrier | `f = f_xosc / 2^16 × FREQ` | `FREQ2:FREQ1:FREQ0` | `10 13 B1` → 417.99991 MHz | `10 13 CF` → 418.01181 MHz |
| Data rate | `R = (256 + M) × 2^E / 2^28 × f_xosc` | `MDMCFG4[3:0]`=E=7, `MDMCFG3`=M=0x83 | **4,797.9 baud** (0.04 % under 4,800) | same |
| Channel filter | `BW = f_xosc / (8 × (4 + M) × 2^E)` | `MDMCFG4[7:4]` = 6 | **270.8 kHz** | same |
| Deviation | `f_dev = f_xosc / 2^17 × (8 + M) × 2^E` | `DEVIATN` | `0x47` → **±47.6 kHz** | `0x45` → ±41.3 kHz |
| Offset compensation limit | `BW/8, BW/4, BW/2` by `FOCCFG[1:0]` | `FOCCFG = 0x16` | **±67.7 kHz** | same |

- **Data rate 4,797.9 vs 4,800.** 0.04 % is far inside the tolerance of an asynchronous link: over ten bits the
  accumulated error is 0.4 % of one bit.
- **Why 270.8 kHz of filter.** A 2-FSK signal needs about `2 × (deviation + baud/2)` (Carson's rule): for the widest
  measured sender `2 × (51 + 2.4) = 107 kHz`. The senders' centres differ by up to ±21 kHz from the receiver's, which
  adds about 42 kHz of margin: roughly 150 kHz at least. 270.8 kHz is the proven preset's value and is kept because it
  was proven; the chip's 162, 203 and 232 kHz steps would also satisfy this estimate and might be a little more
  sensitive, but are untried. CALC, checked in `profiles: the filter is wide enough...`.
- **Why offset compensation matters.** The packs sit up to about 21 kHz away from the receiver's centre. With
  compensation off the demodulator's slicer is wrong by that much and bits fail; `FOCCFG = 0x16` lets the chip track
  up to ±67.7 kHz. After the frequency this is the most important receiver setting. Checked in
  `math: the packs' worst carrier offset fits inside the compensation limit`. The profile also clears
  `FOC_BS_CS_GATE` (the preset's value), so the compensator runs continuously rather than only while a carrier is
  sensed; the datasheet warns it may drift toward its limit when it is fed noise. That is a candidate cause if the
  first frame after a long idle is received worse than the following ones (BRINGUP step 5).
- **Why two profiles.** *Proven* is derived from the configuration packs actually acknowledged (carrier 418.000 MHz,
  ±47.6 kHz), so it is the default. *Matched* reproduces the HSDU's own measured signal (418.012 MHz, ±41.3 kHz, the
  step nearest the measured ±42.5 kHz). It is better in principle and untried on a pack. `profile matched` in the
  console switches between them for an A/B test.

### 3.3 The differences from the proven preset (DESIGN, UNPROVEN)

The proven registers were run on a Flipper Zero's board, not on a bare module. The changes:

| Change | Why |
|---|---|
| Frequency registers added | the Flipper sets the frequency another way |
| `IOCFG0 = 0x2E`, `IOCFG2 = 0x0D` | the Flipper's preset makes GDO0 an output in receive, which would fight the UART's transmit pin |
| `MCSM1.CCA_MODE = 0` | otherwise the chip's clear-channel check can silently refuse to transmit while a pack's carrier is up |
| PATABLE lowered | the preset loads a high power step; the default here is the lowest in the table (section 7) |
| `SIDLE` before every `STX`/`SRX` | belt and braces for the clear-channel check, and every state change starts from a known state |

Seven further registers (`FSCAL3..0`, `TEST2..0`) carry TI's SmartRF recommended values. They are tagged
`kSmartRf` in `profiles.cpp` and a test insists they are the only ones outside the proven list. The datasheet does
not itself give these values (it points to the SmartRF Studio tool), and they are for the 433 MHz band: **verify them
by generating the register set for 418.0 MHz, 4.8 kBaud, 2-FSK, ±47.6 kHz, 270 kHz, asynchronous serial in
SmartRF Studio and comparing** (BRINGUP step 1).

`FSCAL3`, `FSCAL2` and `FSCAL1` are calibration *result* registers: the chip overwrites their low bits on every
calibration. `selftest` therefore compares only their input bits (and reports a synthesizer that did not lock,
`FSCAL1 = 0x3F`), a point the first version of this code got wrong and the review caught.

### 3.4 State changes and timing (DATASHEET)

The chip moves between IDLE, RX and TX by command strobes (`SIDLE`, `SRX`, `STX`). With `MCSM0.FS_AUTOCAL = 1` it
calibrates its frequency synthesizer on every IDLE→RX/TX change: calibration takes about 0.72 ms and the whole switch
about 0.8 ms (datasheet transition table, 799 µs). The driver issues the strobe and **polls `MARCSTATE` until the chip
reports the new state** (with a timeout), so when `enterTx()` returns the carrier is up and the PLL is locked. A
TX→RX turn-around is therefore about 0.8 ms, plus the carrier hold in 4.3, against the 180 ms the packs allow.

**Transmit latency.** The datasheet (Table 10, "TX latency 8 bit, serial operation") gives the transmitter 8 bit periods
(1.7 ms at 4,800 baud) between sampling the data pin and the RF output. When the UART's shift register empties, the last bit is therefore still inside the chip.
The modem keeps the carrier up for at least 3 ms after the UART goes idle, in every key mode (`txLatencyHoldUs`).

### 3.5 Receiving

In receive the chip's demodulator output appears on GDO2 continuously. There is no carrier-detect gating in this
mode, so noise produces random bytes when the channel is empty, exactly like the HSDU. The PC program ignores them.

## 4. The modem (`core/src/modem.cpp`)

### 4.1 State machine

```
              RTS up, or bytes arrive (mode permitting), and the radio is ready
   ┌────────────────────────────────────────────────────┐
   │                                                    ▼
┌──┴───┐   chip reports TX    ┌─────────┐ lead elapsed ┌──────────┐ nothing left, UART idle,  ┌─────────┐
│  Rx  │◄──────────────────── │ TxLead  │─────────────►│ TxData   │────────RTS low───────────►│ TxTail  │
└──────┘  tail elapsed /      └─────────┘              └──────────┘◄── bytes or RTS again ───┴────┬────┘
    ▲     watchdog / fault                                                                         │
    └──────────────────────────────────────────────────────────────────────────────────────────────┘
```

- **Rx**: receiving; bytes from the chip go to the PC.
- **TxLead**: the chip reports TX; wait for the lead-in before releasing any byte.
- **TxData**: feed bytes into the UART as it has room; echo each to the PC.
- **TxTail**: nothing left to send; hold the carrier for the tail (at least 3 ms), then return to Rx.

### 4.2 Three key modes

| Mode | Keys on | Lead before first byte | Tail after last byte | Use |
|---|---|---|---|---|
| `rts` (**the firmware's default**) | RTS high | only the 1 ms PLL settle: the PC waits | 3 ms (the chip's latency) | built to drop in for a host that keys by RTS, as it would the real HSDU; bytes alone never transmit |
| `data` | the first byte | 85 ms added | 80 ms added | a host that cannot set RTS, or Windows dropping a lone RTS change |
| `either` (the library's default; stored-in-flash fallback) | RTS or data | 85 ms only if keyed by data | 80 ms only if keyed by data | works with both, but any program or OS service that opens the port and writes would transmit |

Whether Windows delivers a lone RTS change over USB to the firmware is **UNPROVEN**: it is checked in BRINGUP step 3
(`tools/openhsdu_bench.py rts`). If it does not arrive, `keymode either` (stored in flash) is the fallback. The firmware
defaults to `rts` because byte-keyed modes would let any program, or an operating-system service that probes new serial
ports, put a transmission on the air by merely writing to the port.

### 4.3 Rules the code enforces (each has a test)

1. **The radio must be declared ready.** The modem starts unable to key; only `Device::begin()` succeeding (chip
   found, registers read back, power step allowed, chip receiving) enables it, and a failed profile or power change
   disables it again. A chip that did not take its settings can never be keyed.
2. **The chip is in TX before any byte reaches the wire**, and stays there until the last bit has left *and* for the
   chip's own transmit latency afterwards (3.4).
3. **Bytes are never invented, reordered or sent twice.** Every byte the PC writes is sent once in order, or counted
   in exactly one drop counter (`tx + noKey + full + fault == written`).
4. **Echo** is generated as each byte is handed to the UART, in order.
5. **Received bytes are discarded while transmitting and for 3 ms after** returning to receive (the chip is still
   switching and its data pin is meaningless); the UART's receive queue is flushed at the same moment.
6. **Watchdog.** The carrier never stays up longer than `maxTxUs` (2 s) from the moment the chip reports TX. After a
   trip the PC must release RTS once before RTS can key again, and a fresh RTS request after that is honoured. Keying
   by *bytes* additionally needs the PC to stop sending for a second (`tripQuietUs`; bytes sent in that time are dropped
   and extend it), so a host that streams bytes forever gets one 2 s carrier, not one every 2 s. Bytes still queued in
   the UART are aborted so they cannot leak into the next transmission (a bug the randomised test found).
7. **A chip that says TX when the modem thinks it is receiving is told to receive.** While receiving, the modem asks
   the chip every 20 ms (`transmitting()`, a `MARCSTATE` read, not the modem's own belief). A lost strobe
   therefore ends within about 20 ms, and a failed return to receive is retried every 20 ms.
8. **Withdrawn requests are dropped.** If bytes were accepted while RTS was up and RTS fell before the radio was
   keyed, they are discarded and counted (also found by the randomised test).
9. **Fault back-off.** If the chip fails to enter TX, queued bytes are dropped and counted, the chip is told to
   receive, and no new key-up is attempted for 100 ms. Bytes the PC sends while the chip cannot be confirmed in
   receive, or during that back-off, are dropped and counted too, not queued to go out whenever it recovers.
10. **A closed port stops everything for it.** If DTR is low nothing is queued for the PC, and queued bytes are cleared
    when the port closes. If the USB cable is pulled or the bus suspends, both lines read low, which drops RTS.
    RTS is also not trusted for 50 ms after the port opens or its baud rate changes (`holdRts`), so a terminal that
    raises RTS before it sets 115200 baud cannot key the radio; a real host waits about 80 ms and is unaffected.
11. **No timer outlives its purpose.** Everything that is only true for a while (the receive settle window, the fault
    back-off, an expired bench carrier, the watchdog hold-off) is a flag cleared by a housekeeping step as soon as its
    time is up, never a stored timestamp compared much later. A 32-bit microsecond counter wraps every 71.6 minutes and
    a signed comparison goes wrong after 35.8, which made a receiver deaf and could re-key a carrier in the first
    version (found by the review, tested with 40-, 72- and 100-minute idles). Timestamps that are only read while the
    modem is keyed are bounded by the 2 s watchdog; the one compared on every receive tick (the 20 ms chip check) is
    re-armed if it ever reads as 'in the future'.

### 4.4 Why the bytes do not need real-time handling

A host writes the whole frame to the serial port at once. USB delivers it in packets within a millisecond or so; the
firmware copies it into a 1,024-byte buffer and feeds the UART's 128-byte FIFO as space opens. The UART, not
software, then spaces the bits at 208 µs. A frame can only be damaged by a *gap* in the middle, which would need
the buffer to run dry before the frame ends, and the frame is already entirely in it. Frames longer than the buffer
(1,024 bytes, over two seconds of air time) are not part of this link: the first 1,024 bytes are accepted and the rest
dropped and counted (and the watchdog would cut such a frame at 2 s anyway).

## 5. The ESP32-S3's USB (DATASHEET of the SoC and framework source, UNPROVEN on Windows)

- **Use USB-OTG (TinyUSB) mode**, not the chip's built-in "Hardware CDC and JTAG". In that mode the chip resets
  itself when RTS is high with DTR low, and firmware cannot read the control lines at all. The build removes the
  board definition's `ARDUINO_USB_MODE=1` and sets `=0`.
- **There is one USB serial port, not two.** The Arduino-ESP32 framework (2.0.x) rejects a second CDC interface. The
  console therefore shares the port, and **the baud rate the host has set chooses which it is**: exactly 115200 is the
  text console, anything else is the HSDU data port (see [SERIAL-INTERFACE.md](SERIAL-INTERFACE.md)). The baud rate is
  not carried over the air, so the setting is free to mean this. In console mode the modem is told DTR and RTS are low,
  and RTS is distrusted for 50 ms after any open or baud change, so opening the console can never key the transmitter.
  A program that never sets a baud rate leaves TinyUSB's power-up default of 115200 and so gets the (silent) console.
  The baud rate is read from TinyUSB's stored line coding on every pass: the framework's own change event misses a
  second session at the same rate, which the review caught.
- **DTR and RTS are polled** from TinyUSB each pass (`tud_cdc_n_get_line_state`), so an unplugged or suspended bus,
  which reads as both low, is handled by the same code as a normal close.
- **The PC must hold DTR on**, or the board sends nothing back.
- **`enableReboot(false)`**: otherwise a DTR/RTS pattern reboots the board into its loader. A consequence: every
  re-flash needs BOOT held while plugging in.
- **Windows and RTS-only changes.** Windows' USB serial driver is reported to drop a change to RTS alone while the
  call .NET makes (`RtsEnable`) is reported to work. Not bench-tested; hence key mode `either`.
- **A hardware backstop.** The loop task watchdog (1 s) resets the ESP32 if `loop()` ever stops. The CC1101 keeps
  transmitting during that reboot until the new boot resets it (about half a second): the modem's own 2 s watchdog is
  cooperative, and this is the limit of what software on one chip can promise.

## 6. Where the chip's pins go

See [HARDWARE.md](HARDWARE.md). The two serial pins are on the ESP32's UART1 (GPIO8 transmit, GPIO9 receive), the
SPI pins on GPIO10–13. GPIO 0, 3, 45, 46 (boot straps), 19, 20 (USB) and 21 (LED) are never used; a test checks it.
The UART's transmit pin is connected only after the chip's own pin has been set to high impedance, because at power-on
the chip drives GDO0.

## 7. Power, range and the law (CALC)

See [REGULATORY.md](REGULATORY.md) for the rules. The arithmetic the defaults rest on:

- **Radiated power** ≈ chip output − attenuator + antenna gain. `EirpBudget` in `power.h`.
- **The 47 CFR 15.209 general limit** is a field strength of 200 µV/m at 3 m. As EIRP: `P = (E·d)² / 30 =
  (200e-6 × 3)² / 30 = 12 nW = −49.2 dBm`. Test: `math: field strength to EIRP`.
- **The chip's lowest steps** are about −30 to −37 dBm at the pin (TI DN013, typical; DN013 lists a few other very low
  steps that this project's table does not include), so an inline 20 dB attenuator is needed to get under −49 dBm. The
  default step, the lowest in this project's table, `0x01` (−36.5 dBm nominal), with a 20 dB attenuator and a 2 dBi
  antenna is −54.5 dBm: **5.3 dB under the limit**. The loudest allowed step, `0x11` (−33.8 dBm), leaves **2.6 dB**. The
  next step up, `0x12` (−30 dBm), would be 1.2 dB over, so the default build refuses it. All of this is in
  `test_power.cpp`. Meeting the field-strength limit is necessary, not sufficient (REGULATORY.md).
- **This does not see radiation from the board and wiring**, which can exceed what the attenuated antenna feed
  carries. The real level has to be measured once.

**Range** (free space, 0 dBi pack antenna, 10 dB margin; `d = c/(4πf) × 10^((EIRP − sensitivity − margin)/20)` at
418 MHz). The pack's own sensitivity is unknown, so two *assumed* values are used, −100 dBm and −107 dBm (neither is
measured; they bracket what simple receivers of this kind are usually specified at):

| Base EIRP | Free-space range | With 10 dB of body loss |
|---|---|---|
| −49.2 dBm (the 15.209 limit) | 6.3 to 14 m | 2.0 to 4.4 m |
| −51.8 dBm (loudest allowed step) | 4.6 to 10 m | 1.5 to 3.3 m |
| −54.5 dBm (**the firmware's default**) | **3.4 to 7.6 m** | 1.1 to 2.4 m |

These are computed, not measured, and the default is the bottom row. **The range walk in BRINGUP.md is the deciding
test**, and if one base does not cover an arena the answer is several low-power bases, not one high-power one.

**The attenuator also costs 20 dB on receive.** It sits between the module and the antenna and is the same in both
directions, so the base hears the packs 20 dB less well: the chip's roughly −100 dBm sensitivity becomes about
−80 dBm at the antenna. By these numbers the uplink matches the downlink at the default step if a pack radiates at
least about −35 dBm EIRP; **the pack's transmit level is unmeasured (UNPROVEN)**, so this is a risk, not a result.
The `rssi` console command reads the received level, so BRINGUP step 6 measures the uplink margin directly.

## 8. Failure modes and where they are covered

| Symptom | Cause | Handled / tested |
|---|---|---|
| Carrier stays on | host crashed holding RTS; USB pulled; stuck byte stream | watchdog and hold-off, line-state polling; `modem`, `hardening`, `fuzz` suites |
| Carrier on with no request | lost strobe, glitch | the 20 ms `transmitting()` check; `hardening` suite |
| Stale bytes at the start of the next frame | RTS dropped before key-up; carrier cut with bytes in the UART | dropped and counted; `abortTx`; found by `fuzz` |
| End of the last byte missing | carrier dropped before the chip's 8-bit latency | 3 ms hold; `hardening` suite |
| First bytes lost | sent before the PLL locked | `radioSettleUs` and `MARCSTATE` polling; `modem` suite |
| Transmits from a half-configured radio | failed boot or profile change | the ready gate; `hardening` suite |
| Receiver silent after 36 minutes idle | a stored timestamp compared after a wrap | flags cleared by housekeeping; `hardening` suite |
| Reply lost right after transmit | receiver still switching | 3 ms settle; `integration` suite |
| `selftest` fails on a healthy chip | calibration rewrote FSCAL3/2/1 | masked compare; `hardening` suite |
| Radio missing or SPI wrong | wiring | `selftest` reads PARTNUM/VERSION and the profile's registers back |
| Packs do not respond | frequency, deviation, or level wrong | bring-up steps 2 to 5 against the SDR and a pack |
| A console session keys the radio, or a later one lands in data mode | the framework's baud event is filtered; RTS raised before the baud arrives | baud read from TinyUSB every pass; 50 ms RTS hold-off; `host_mux` and `hardening` suites |
| A polling host is locked out after one stall | the hold-off refreshed by every dropped byte | a fresh RTS request clears it; `hardening` suite |
| No serial port appears | image built for 8 MB flash on a 4 MB chip | `tools/check_firmware_image.py` in CI; HARDWARE.md |
| Board resets when the port opens | wrong USB mode | build flags; section 5 |
| Receives garbage | polarity or baud wrong | polarity needs no inverter; baud checked by `math` suite |

## 9. What is proven and what is not

**Proven (LIVE, on real packs, through a Flipper Zero's CC1101 running its own preset):**
- the link parameters in section 2;
- that a CC1101 with that preset transmits frames real packs acknowledge and receives their replies;
- that a base which only transmits can start a game on a pack.

**Proven in software (this repository's tests):** the equations, and the carrier, baud rate, deviation and filter that
the registers decode to, against the measured link (the other registers against the preset's published values);
the driver against a model of the chip; every modem rule in section 4.3, including under randomised event orders and
lost-strobe faults and across long idle periods; the console and the one-port multiplexer; the bench tool; the
firmware compiles for the ESP32-S3 with a 4 MB image.

**Not proven (needs the parts in hand):**
1. that an ESP32-S3 UART driving GDO0 produces a signal packs accept (the timing argument in 3.1 says it should);
2. that the SmartRF extra registers are right for this link at 418 MHz;
3. that this register set (with its five changes) works, and that a bare module at 418 MHz performs as well as the
   Flipper's board (some modules' filters are tuned to 433 MHz);
4. that a lone RTS change reaches the firmware on Windows;
5. the real transmitted level, the range at it, and the uplink through the attenuator;
6. that the framework calls in `firmware/src/main.cpp` behave on silicon as the framework source says they do;
7. anything about a second, per-pack Wi-Fi path (not part of this repository).

## 10. Glossary

**EIRP** equivalent isotropically radiated power. **FSK** frequency-shift keying. **GDO** the CC1101's general
digital output pins. **HSDU** the base-station radio unit. **LSB first** the least significant bit is sent first.
**Mark / space** the two states of a UART line: 1 / 0. **PATABLE** the chip's power table. **PLL** phase-locked loop,
the chip's frequency synthesiser. **PTT** push to talk. **RTS / DTR** serial control lines; here RTS means "transmit"
and DTR means "the host has the port open". **SDR** software-defined radio, used as a test receiver.
