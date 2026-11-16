# Testing

## Run everything

```
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure   # host tests
cmake -S . -B build-san -DOPENHSDU_SANITIZE=ON && cmake --build build-san && ./build-san/openhsdu_tests
python3 -m pytest tools                                                                     # bench tool, image check
(cd firmware && pio run) && python3 tools/check_firmware_image.py firmware/.pio/build/esp32s3zero
python3 tools/mutate.py --count 0 core/src/modem.cpp core/src/device.cpp ...               # mutation testing (slow)
```

CI (`.github/workflows/ci.yml`) runs the first four on every push.

## What each suite proves

| Suite | File | What it checks |
|---|---|---|
| `math` | `test_math.cpp` | each CC1101 equation against the measured link: 418.000 and 418.012 MHz, 4,797.9 baud, 270.8 kHz, ±47.6 and ±41.3 kHz, the offset-compensation limit against the packs' worst offset, EIRP −49.2 dBm, wrap-safe time |
| `profiles` | `test_profiles.cpp` | register values equal the proven preset; every register is tagged by where it came from; bit fields; both profiles decode to their stated intent; matched differs from proven by exactly two registers |
| `power` | `test_power.cpp` | table order and uniqueness; default and cap; every allowed step is under the 15.209 limit with the stated attenuator; the next step up is not; the margins |
| `ring_buffer` | `test_ring_buffer.cpp` | order, capacity, wrap, clamping, long cycling |
| `driver` | `test_driver.cpp` | SPI header bytes; reset sequence; identification; apply with read-back and every failure; strobe order (idle first); calibration time; timeouts; glitched status reads; dead chip |
| `modem` | `test_modem.cpp` | RTS, data and either key modes with measured lead and tail; first byte waits for the PLL; long frames; RTS stuck; watchdog; overflow; radio faults and recovery; receive forwarding, settle, DTR, blocked and partial USB; carrier and pattern helpers; clock wrap |
| `console` | `test_console.cpp` | every command, every limit, refusal while transmitting, line editing |
| `host_mux` | `test_host_mux.cpp` | exactly 115200 baud is the console; opening the console cannot key the radio; mode switches drop and restore the lines; console text delivered in order through a slow USB side |
| `hardening` | `test_hardening.cpp` | one regression test per confirmed core-logic finding of the two review rounds (below) |
| `mutation-kills` | `test_mutation_kills_*.cpp` | tests written specifically to kill mutants the earlier suite let survive: exact timer boundaries, the device's own policy checks (not just the console's), DTR handling |
| `integration` | `test_integration.cpp` | twenty request/reply exchanges as a host keyed by RTS does them; turn-around 3 to 5 ms; a reply right after transmit; no-squelch noise; mode switching; cable pulled mid-frame |
| `fuzz` | `test_fuzz.cpp` | 480 randomised event sequences (random PC and radio events, USB back-pressure, lost strobes, random clock start) against five invariants (below) |
| `wiring` | `test_pins.cpp` | no forbidden or duplicate pins |
| bench tool | `tools/test_openhsdu_bench.py` | the RTS and echo checks against a fake board, including the failure messages and that RTS is set low before the port opens |
| image check | `tools/test_check_firmware_image.py` | the 4 MB flash check, on synthetic images |

### The five invariants the randomised test enforces

1. a byte only ever *starts* on the wire while the chip is transmitting;
2. the carrier never exceeds the watchdog limit plus one tick;
3. every byte the PC wrote is either sent once in order or counted in exactly one drop counter (`tx + noKey + full +
   fault == written`);
4. the PC only receives echoes of what was sent, in order, and bytes the radio produced, in order;
5. after the inputs stop the modem is back in receive, the chip is receiving, and the UART is idle.

## Automated review (2026-11-16): two rounds, and what they found

Before this repository was published it was reviewed twice by automated agents (AI agents, not an outside human audit),
each round from independent angles, and a further agent then tried to refute every serious finding. Round one: the TI
datasheet against the code and the chip model; the firmware against the Arduino-ESP32 and ESP-IDF source; an adversarial
read of the core logic; mutation testing; a fact-check of every document; a public-release privacy audit; a hardware and
pin review. Round two re-reviewed the fixes, checked the documents against the new code, and audited the exact tree and
history to be published. Confirmed findings, all fixed. Every core-logic finding has a regression test that failed
before the fix (checked against the earlier code for round two); the *firmware*-only ones are verified by compilation
and by bring-up only.

**Round one**

| Finding | Fix |
|---|---|
| The second USB serial port the console needed cannot exist in this framework (*firmware*) | one port; the baud rate selects console (115200) or data (`HostMux`) |
| The build assumed 8 MB flash; the chip has 4 MB, so the board would not boot and no port would appear (*firmware*) | 4 MB flash and partition table; `tools/check_firmware_image.py` in CI |
| A failed `Device::begin()` was ignored: the modem could still key an unverified radio | the modem starts unable to key; only a successful begin enables it |
| `selftest` compared FSCAL3/2/1, which are calibration results the chip rewrites, so it would fail on a healthy chip | compare input bits only; model calibration in the chip model |
| The carrier was dropped as soon as the UART emptied, cutting the end of the last byte (8-bit modulator latency) | 3 ms minimum hold in every key mode |
| After 35.8 minutes idle the receiver silently discarded everything; a fault back-off and an expired bench carrier could outlive their purpose and key the radio | timers are flags cleared by housekeeping, never a stored timestamp compared later |
| A host streaming bytes could defeat the 2 s watchdog by re-keying within a tick | data keying held off until the PC has been quiet for 1 s |
| A key-up that failed half way, with a lost recovery strobe, could leave the chip transmitting with nothing noticing | the modem reads the chip's own state every 20 ms and retries a failed return to receive every 20 ms |
| UART receive thresholds left the driver defaults, delivering bytes late and in batches (*firmware*) | threshold and timeout set to one |
| The UART transmit pin was connected while the chip's GDO0 was still an output (*firmware*) | connected only after the chip is configured |
| Two chip-model reset values were wrong; nothing pinned them | corrected and pinned against the datasheet |
| Documents: an overclaim about what was proven, a contradiction about provenance, range figures for a power the firmware does not use, the attenuator's receive-side cost left out | corrected throughout |

**Round two**

| Finding | Fix |
|---|---|
| The framework's baud-rate event fires only when the rate *changes*, so a second console session at 115200 landed in data mode and its RTS keyed the radio (*firmware*) | the baud rate is read from TinyUSB's stored line coding on every pass |
| A terminal that raises RTS before its baud rate arrives could key the radio for a few ms | RTS is distrusted for 50 ms after the port opens or its baud rate changes (`holdRts`), shorter than a host's usual 80 ms wait |
| After one watchdog trip, a host that kept polling was locked out for good, because every dropped byte renewed the hold-off | a fresh RTS request clears it; only a stream of bytes with no RTS stays blocked |
| `setProfile` could enable the modem after a failed `begin()`, when the transmit pin was never connected | `begin()` must have succeeded; refused otherwise |
| The firmware keyed on bytes by default, so any program or OS service that opened the port and wrote would transmit (*firmware*) | the firmware defaults to key mode `rts`, stored in flash |
| Bytes written while the chip could not be confirmed in receive were queued and sent whenever it recovered | dropped and counted |
| A half-typed console line survived a port close and was completed by the next session | forgotten on close and on mode switch |
| The 20 ms stuck-carrier check could stay off for 35 minutes after a loop stall | re-armed when it reads as in the future |
| `begin()` could reset the chip under a live carrier | refused while keyed |
| Documents: `carrier` duration wrong by 85 ms, `pattern` echo wrong, the five/four interfaces, 15.203 stated too softly, overclaims about what the tests cover | corrected |

Earlier still, the randomised test itself found two bugs while the code was being written: stale bytes leaking into the
next frame after a forced carrier cut, and bytes accepted just before RTS fell.

## Mutation testing

`tools/mutate.py` changes one line at a time (a comparison, `&&`/`||`, a constant, a deleted statement), rebuilds, and
runs the suite; a mutant is *killed* if any test fails. On the final code (2026-11-16, Apple clang, Debug) the runner
found 318 candidate mutants in the seven core files that carry logic (`modem`, `device`, `cc1101`, `console`,
`host_mux`, `cc1101_math`, `power`). 79 did not compile and are discarded; **all 239 valid mutants were killed**. Two of
those survived the runner's first pass and were killed when it re-checked them (the first pass has shown the odd false
survivor, so the runner re-runs every survivor and believes one only if it survives twice).

How it got there: an earlier hand-driven pass by a reviewer on the first version (265 mutants, 72 % killed) produced the
55 `mutation-kills` tests; the first run of the runner on the fixed code killed 219 of 226 valid mutants (96.9 %), and
its survivors showed real gaps (a chip-select line never released, which the chip model could not see and now counts; a
flag left set after a failed profile change; an unpinned post-reset margin), each now tested.

Mutation testing measures whether the tests notice a changed line of code; it says nothing about whether the model of
the chip is right (see below).

## Sanitizers and coverage

The whole suite also runs clean under AddressSanitizer and UndefinedBehaviorSanitizer (`-DOPENHSDU_SANITIZE=ON`).
Line coverage of `core/src` (llvm-cov gcov on Apple clang, Debug build): `cc1101` 99.3 %, `cc1101_math` 100 %, `console`
98.3 %, `device` 100 %, `host_mux` 100 %, `modem` 98.5 %, `power` 100 %, `profiles` 100 %. The uncovered lines are
defensive branches (a few display-only formatting branches, the last retry of the glitched-status read).

## What is *not* tested here, and what covers it instead

| Gap | Covered by |
|---|---|
| the ESP32 glue (`firmware/src/main.cpp`): UART thresholds and FIFO arithmetic, the SPI transaction wrapper, TinyUSB line-coding and line-state polling, the transmit-pin connect order, `enableReboot`, the flash-stored key mode | compiled for the target and the image checked in CI; it is thin by design; verified on hardware in BRINGUP steps 1, 3 and 4 |
| the chip's real behaviour (calibration time, pin behaviour, SmartRF registers, transmit latency) | BRINGUP steps 1 and 2 |
| RF: frequency, deviation, level | BRINGUP step 2 with an SDR and a calibrated measurement |
| whether packs accept the signal, uplink through the attenuator, range | BRINGUP steps 5 to 7 |
| Windows' handling of RTS | `tools/openhsdu_bench.py rts` |

The `MockCc1101` is a model written from the datasheet. A test that passes against it shows the code is consistent
with the datasheet as read, not that the chip agrees; the bring-up steps exist to close exactly that gap. The
review's datasheet reviewer found the model wrong in three places (reset values, calibration results, status codes),
which are fixed, and it is stricter than the chip in one (it ignores `STX`/`SRX` issued outside idle, because the
driver always goes through idle first).

## Adding a test

Use `sim::Bench` (`sim/bench.h`): it wires a modem, driver, chip model, UART and host to one fake clock. `run(us)`
plays the firmware loop, `lines()`, `pcWrite()` and `radioRx()` play the PC and the radio, and `lqSend()` plays what a
host keyed by RTS does for each frame. Assert on `uart.sent` (every byte with its time on the wire), `chip.history`
(every state change) and `host.received`. For a regression test of a timing bug, drive the clock by hand with
`clock.advance()` and `modem->tick()` so the boundary is hit to the microsecond.
