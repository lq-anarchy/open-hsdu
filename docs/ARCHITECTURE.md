# Architecture

## One idea

Everything that has logic in it is plain C++17 with no hardware calls, written against six small interfaces
(`core/include/openhsdu/hal.h`; the firmware implements four of them (`ISpi`, `IClock`, `IUart`, `IHostLink`); `IRadio` is the chip
driver and `IConsoleOut` is `HostMux`, both in the core).
The ESP32 firmware implements them on real hardware; the tests implement them with a model of the chip.
**The code that runs on the board is the code that is tested**, not a copy of it.

```
                       core/  (portable C++, tested on a PC)
   ┌───────────────────────────────────────────────────────────────────────────────────┐
   │                                                                                   │
   │   USB port ──► HostMux ──(baud != 115200)──► Modem ──IRadio──► Cc1101 ──ISpi──► chip
   │      ▲            │                           │  ▲               ▲                │
   │      │            └──(baud == 115200)──► Console ─► Device ──────┘                │
   │      │                    ▲                   │                                   │
   │      └──── IHostLink ◄────┴──── console text, echo, received bytes                │
   │                                                                                   │
   │   UART ──► Modem.onUartRx            Modem ──► IUart.write ──► UART ──► chip GDO0 │
   └───────────────────────────────────────────────────────────────────────────────────┘
   firmware/src/main.cpp implements ISpi, IClock, IUart, IHostLink and the loop on the ESP32-S3
```

## Modules

| File | What it is | Depends on |
|---|---|---|
| `cc1101_regs.h` | register map, strobes, status registers, MARCSTATE values | nothing |
| `cc1101_math.{h,cpp}` | the chip's equations (carrier, baud, filter, deviation), EIRP, wrap-safe time | nothing |
| `profiles.{h,cpp}` | register lists (`proven`, `matched`) and the intent each is meant to produce | regs |
| `power.{h,cpp}` | allowed PATABLE steps, the default and the cap, the EIRP budget | math |
| `hal.h` | the six interfaces | nothing |
| `ring_buffer.h` | fixed-size byte FIFO, no heap | nothing |
| `cc1101.{h,cpp}` | SPI framing, reset, identification, apply-with-readback, enter TX/RX | hal, regs, math, profiles |
| `modem.{h,cpp}` | the PTT/echo state machine | hal, ring buffer, math |
| `device.{h,cpp}` | boot sequence, power policy, self-test | cc1101, modem, power |
| `console.{h,cpp}` | text commands | device, modem, power |
| `host_mux.{h,cpp}` | one USB port, two jobs: baud 115200 = console, anything else = data port; quiets the modem in console mode | console, modem |

`firmware/src/pins.h` holds the wiring as constants so a host test can check it. `sim/` holds the test doubles:
`MockCc1101` (registers, strobes, a state machine with the calibration delay, fault injection), `FakeClock`,
`FakeUart` (paces bytes at 4,800 baud and records when each was on the wire), `FakeHost`, and `Bench`, which wires a
whole device together and plays the PC.

## Decisions worth knowing

1. **The modem is protocol-blind.** It never parses a frame. That keeps it small, makes it safe to publish, and means
   a change to the game's protocol never touches it.
2. **Time is injected.** `Modem` reads `IClock`; nothing calls `millis()`. A test that needs 2 seconds of watchdog
   takes microseconds, and the same code is tested across the 32-bit wrap.
3. **No heap in the hot path.** Buffers are fixed. The console formats into stack buffers.
4. **Driver waits for the chip's own report.** `enterTx()` returns only when `MARCSTATE` says TX; the modem trusts
   that and does not guess at calibration time.
5. **Power is policy, not a parameter.** `Device` refuses a PATABLE byte that is not in the table or is above the
   cap, including at boot. The cap is a build option.
6. **The UART is fed by hand into its hardware FIFO** (`uart_ll_write_txfifo`), never through a blocking driver
   call, so the loop never stalls and `writable()`/`busy()` are exact.
7. **One USB port, two jobs.** The framework supports only one serial port, so the baud rate chooses: 115200 is the
   console, anything else the data port. In console mode the modem sees DTR and RTS low, and RTS is distrusted for
   50 ms after any open or baud change, so merely opening the console can never key the radio (the `carrier` and
   `pattern` commands key it deliberately, time-limited).
8. **Fail towards silence.** The modem cannot key until `Device::begin()` has verified the radio. Every error path ends
   with the carrier down and the chip receiving, and queued bytes counted rather than sent late. The modem asks the chip
   (not its own belief) whether it is transmitting every 20 ms while it should be receiving.
9. **No stored timestamp is compared later.** Timers that are only true for a while are flags cleared by a housekeeping
   step, because a signed 32-bit comparison goes wrong after 35.8 minutes.

## What the loop does (`firmware/src/main.cpp`)

1. poll the USB state (ready, DTR, RTS) and the baud rate the host set, and pass them to the mux;
2. read bytes from the USB port into the mux (console or modem);
3. read bytes from the UART into the modem;
4. `modem.tick()` (all timing and keying) and `mux.poll()` (flush console text);
5. `yield()`; the loop watchdog is fed by the framework's loop task.

## Adding a feature

Put the logic in `core/`, give it a test with the fakes, and only then touch `firmware/`. If it needs hardware the
interfaces do not offer, extend `hal.h`, the fakes in `sim/`, and the ESP32 classes together.
