# What the PC sees

The board appears as **one USB serial port**. The Arduino-ESP32 framework supports only one, so it does two jobs and
**the baud rate the host has set chooses which**:

| The host has the port at | You get |
|---|---|
| **115200 baud, exactly** (also TinyUSB's power-up default) | the text console (`help`) |
| **any other baud rate** (a host program for the real HSDU uses 4800) | the HSDU data port |

The baud rate is not carried over the air, so the setting is free to mean this. **A program must set a baud rate other
than 115200 to get the data port; one that never sets a baud rate gets the console**, which is silent: it cannot key the
transmitter. `python3 tools/openhsdu_bench.py find` lists the port by its name, `open-hsdu`.

Opening the console cannot key the transmitter: in console mode the firmware treats DTR and RTS as low whatever the
program sets, and RTS is not trusted at all for 50 ms after the port opens or its baud rate changes (a terminal raises
RTS before it sets the baud rate). Going back to the data port restores whatever the lines are then.

## Data port: built to behave like an HSDU

| Setting | Value |
|---|---|
| Baud | any except 115200 (the radio side is fixed at 4800) |
| DTR | **must be on.** With DTR off nothing is sent to the PC |
| RTS | on = transmit. Not trusted for 50 ms after the port opens or the baud rate changes; a host that waits its usual ~80 ms after raising RTS is unaffected |
| DSR | not driven. The real HSDU holds DSR high regardless of DTR and RTS, so a host that merely displays DSR is unaffected; one that waits for DSR would not work |
| Key mode | **`rts` by default** (stored in flash; change it with `keymode` on the console): bytes alone never transmit, so no program or operating-system service that opens the port and writes can put anything on the air. `either` and `data` also key on bytes; they are the fallback if RTS does not reach the firmware (BRINGUP step 3) |

| PC action | Result |
|---|---|
| raise RTS | the carrier comes up on the mark tone; bytes written now go out as soon as the PLL has settled (1 ms) |
| write bytes while RTS is up | sent at 4800 baud, in order, each echoed back to the PC as it is handed to the UART |
| write bytes without RTS | key mode `rts` (default): ignored and counted. `data` / `either`: transmitted with an 85 ms lead-in and an 80 ms tail added by the firmware |
| drop RTS | the carrier ends 3 ms after the last queued byte has left (the chip's own transmit latency), then the chip returns to receive in about 1 ms. A byte written just after RTS dropped, while the carrier is still draining, is still sent |
| nothing sent | the receiver's output flows to the PC: random bytes when the channel is empty, a pack's preamble and frame when one answers |
| hold RTS for more than 2 s | the watchdog cuts the carrier. RTS keying needs only one release and a fresh raise to work again. Bytes sent within a second of the trip are dropped and counted (and extend that second), so a stream of bytes with no RTS stays off |
| close the port (DTR low) | nothing is queued for the PC; queued bytes are discarded. The carrier ends if the host also drops RTS (most do on close) |
| unplug or suspend | both lines read low, so RTS drops and any carrier ends |

A host program that keys by RTS the way the real HSDU is driven (raise RTS, wait about 80 ms, write a frame, wait for it
to leave and a little longer, drop RTS) is **expected** to work unchanged: that is the design, and it is untried until
BRINGUP steps 3 and 4 have been done.

## Console: text, with the port at 115200 baud

One command per line (CR, LF or CRLF). `help` lists them. A half-typed line is forgotten when the port closes or
changes mode, so a later session cannot finish it by pressing Enter.

| Command | Does |
|---|---|
| `ver` | version, active profile, key mode |
| `status` | state, DTR/RTS, radio present, counters, drop counters, and a `boot:` line: why the chip last started, the boot count, and the last crash, watchdog or brownout reset kept in flash (a replug counts as a power-on, so this is how a reset before it is still seen), and `usb-stuck=N`: how often the firmware found the USB stack's task not running and restarted the chip to bring USB back |
| `selftest` | reads the chip's identity, reads back the registers the profile sets (calibration-result bits excluded) and PATABLE, checks it is receiving; prints PASS or FAIL |
| `rssi` | the received signal strength register and an approximate dBm (offset 74 dB assumed); use it to compare signals, not to measure them |
| `reg <hex>` / `regs` | read one configuration register / dump all 47 |
| `power [hex]` / `powers` | show or set the PATABLE byte (only allowed steps at or below the build's cap; stored in flash, and a stored step above the cap of the build that boots is replaced by the lowest step) / list them |
| `profile [proven\|matched]` | show or switch the register profile (also works as a retry after a failed change, but not after a failed boot) |
| `keymode [rts\|data\|either]` | show or set the key mode (stored in flash) |
| `carrier [ms]` | key a plain unmodulated carrier on the mark tone: it lasts the time asked (but at least the 85 ms lead-in) **plus** the 80 ms tail, so about N + 80 ms and never less than about 165 ms; max 1000 ms |
| `pattern [n]` | send up to 64 bytes of `0xAA` (the preamble byte): a clean tone pair for an SDR. The bytes go out on the air and are counted in `tx`; their echo is dropped (the port is the console, not the data port) |
| `loader now` | restart into the chip's ROM flash loader, to upload firmware without the BOOT button (docs/HARDWARE.md). Only this exact line does it, and not while transmitting |

The console cannot transmit anything but a carrier or `0xAA`. Commands that touch the chip are refused with
`error: transmitting` while the carrier is up. A line longer than 80 characters, or containing a NUL, is refused whole.

`status` fields: `tx`/`rx` bytes; `keyups`; `watchdog` trips; `faults` (the chip did not change state);
`unexpected-tx` (the chip was found transmitting when the modem thought it was receiving); `dropped:` `nokey` (bytes
without RTS in key mode `rts`, or after RTS was withdrawn), `full` (a buffer was full), `fault` (radio not ready or
failed, watchdog, or the watchdog hold-off), `aborted` (queued in the UART and thrown away when the carrier was cut),
`nodtr` (the PC had not opened the port, or this is console mode), `rx-while-tx` and `rx-settle` (radio bytes ignored
while switching).

Because the console and the data port are the same port, `status` read at 115200 shows DTR/RTS as 0: the modem is told
the port is closed while it is a console. To see whether RTS reaches the firmware use `tools/openhsdu_bench.py rts`,
which checks it by behaviour.

## Windows hosts: two things found on unit 1

- **A lone RTS change does not reach the board.** The Windows CDC driver (usbser.sys) passes RTS on only when DTR is
  written as well. A host that keys by RTS alone gets no transmission in key mode `rts`; use `keymode either` (or
  `data`) and let the data key the radio.
- **Do not change port settings around every frame.** .NET's `SerialPort` sends the whole port configuration to the
  device again (SetCommState) on every `RtsEnable` or `DtrEnable` change. Done twice a frame at four frames a second,
  that stopped the board's USB control channel after 56 to 205 frames: the board stayed listed, every request failed
  with "A device attached to the system is not functioning", and only a replug brought it back. The chip did not reset
  (`boot:` line) and TinyUSB's task kept running (`usb-stuck=0`), so the fault is in the control-request path of the
  USB stack this firmware is built on (Arduino-ESP32 2.0.17) and is still open. The same traffic with RTS left alone
  ran 873 frames, and 879 with RTS switched by `EscapeCommFunction` only (what pyserial does). Set the lines once when
  the port is opened and leave them.
