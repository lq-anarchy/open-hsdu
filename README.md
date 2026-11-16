# open-hsdu

An open replacement for the **HSDU**, the base-station radio that talks to laser-tag packs: an ESP32-S3 and a CC1101
radio module, plugged into a PC by USB.

The HSDU is a modem with a push-to-talk line. This is a modem with a push-to-talk line, built from parts you can buy
today and **built to behave the same way to the program on the PC**: raise RTS, write bytes, they go out as 4800-baud
2-FSK near 418 MHz; whatever the radio hears comes back; transmitted bytes are echoed.

> **Status: v0.1.0, not yet run on hardware.** The radio settings are derived from a configuration real packs have
> acknowledged. The core logic is tested in software (see [docs/TESTING.md](docs/TESTING.md): 259 test cases, randomised,
> mutation and sanitizer runs, and two rounds of automated multi-agent review whose findings are fixed, with regression
> tests for the core-logic ones). The ESP32 glue is thin and untested on a host: it compiles for the ESP32-S3 with a
> verified 4 MB image, and is exercised only in bring-up. The parts are on order.
> [docs/BRINGUP.md](docs/BRINGUP.md) is the path from first power-on to a whole game, and
> [docs/THEORY.md §9](docs/THEORY.md#9-what-is-proven-and-what-is-not) says exactly what is and is not proven.

> **Radio rules.** The link runs near 418 MHz, which in the US is not a licence-free data band. The firmware defaults
> to the lowest power steps and refuses the rest unless you build for a dummy load. Read
> [docs/REGULATORY.md](docs/REGULATORY.md) before you transmit anything, and measure what you radiate.

It is **protocol-blind**: it never looks inside a frame, so it contains no game protocol and is meant to work with
whatever program drives the serial port.

## Quick start

```
git clone https://github.com/lq-anarchy/open-hsdu && cd open-hsdu

# test (no hardware needed)
cmake -S . -B build && cmake --build build && ctest --test-dir build

# build and flash the firmware
cd firmware && pio run -t upload        # hold BOOT while plugging in the cable
```

Wire the module as in [docs/HARDWARE.md](docs/HARDWARE.md), then follow [docs/BRINGUP.md](docs/BRINGUP.md). The board
shows up as **one** serial port: with the host at 115200 baud it is a text console (`help`), at any other baud rate it
is the HSDU data port ([docs/SERIAL-INTERFACE.md](docs/SERIAL-INTERFACE.md)). The firmware keys the transmitter by RTS
only, like the real HSDU; bytes alone never transmit unless you change `keymode`.

## Documentation

| | |
|---|---|
| [docs/THEORY.md](docs/THEORY.md) | how the link works, the chip's equations, the modem rules, range, what is proven |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | modules and the decisions behind them |
| [docs/SERIAL-INTERFACE.md](docs/SERIAL-INTERFACE.md) | what the PC sees: data port and console |
| [docs/HARDWARE.md](docs/HARDWARE.md) | parts, wiring, flashing, flash size |
| [docs/BRINGUP.md](docs/BRINGUP.md) | first power-on to a whole game |
| [docs/TESTING.md](docs/TESTING.md) | the test suites, the invariants, mutation testing, the review, the gaps |
| [docs/REGULATORY.md](docs/REGULATORY.md) | the radio rules and what the defaults do about them |

## Layout

```
core/       portable C++17: CC1101 driver, modem state machine, console, one-port mux, equations (tested on a PC)
sim/        test doubles: chip model, fake clock/UART/host, a whole-device bench
test/       doctest suites (math, profiles, power, ring buffer, driver, modem, console, host mux, hardening,
            mutation kills, integration, fuzz, wiring)
firmware/   ESP32-S3 glue (PlatformIO): SPI, UART, USB, the loop
tools/      bench checks to run from a PC, the firmware-image check, a mutation-testing runner, with tests
docs/       theory, architecture, hardware, bring-up, testing, regulatory
```

## Independence

This project is independent. It is not made by, endorsed by, or affiliated with the makers of the original system.
It contains no vendor software or documents and no game protocol; it describes only the radio link behaviour that was
measured on the air and the serial-port behaviour a host sees. Product and company names belong to their owners.

## Licence

MIT, see [LICENSE](LICENSE). `third_party/doctest` is doctest v2.4.11 by Viktor Kirilov (MIT), unmodified; its licence
is `third_party/doctest/LICENSE.txt`. The software controls a radio transmitter: using it lawfully is your
responsibility (see [docs/REGULATORY.md](docs/REGULATORY.md)).
