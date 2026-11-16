// A whole device on a desk, in software: modem + driver + chip model + UART + host, on one fake clock.
// `run()` plays the part of the firmware's main loop; the helpers below play the PC.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "fake_clock.h"
#include "fake_host.h"
#include "fake_uart.h"
#include "mock_cc1101.h"
#include "openhsdu/cc1101.h"
#include "openhsdu/device.h"
#include "openhsdu/modem.h"
#include "openhsdu/power.h"
#include "openhsdu/profiles.h"

namespace sim {

struct Bench {
  FakeClock clock;
  MockCc1101 chip;
  openhsdu::Cc1101 driver;
  FakeUart uart;
  FakeHost host;
  std::unique_ptr<openhsdu::Modem> modem;
  std::unique_ptr<openhsdu::Device> device;

  explicit Bench(openhsdu::ModemConfig cfg = openhsdu::ModemConfig(), uint32_t clockStart = 1000)
      : clock(clockStart), chip(clock), driver(chip, clock), uart(clock) {
    modem.reset(new openhsdu::Modem(cfg, driver, uart, host, clock));
    device.reset(new openhsdu::Device(driver, *modem));
  }

  bool boot(const openhsdu::Profile& p = openhsdu::profileProven(), uint8_t patable = openhsdu::kDefaultPatable,
            double maxChipDbm = openhsdu::kDefaultMaxChipDbm) {
    return device->begin(p, patable, maxChipDbm);
  }

  // The firmware's loop: advance time in small steps and tick the modem each step.
  void run(uint32_t us, uint32_t stepUs = 100) {
    uint32_t left = us;
    while (left > 0) {
      const uint32_t s = left < stepUs ? left : stepUs;
      clock.advance(s);
      modem->tick();
      left -= s;
    }
  }

  void lines(bool dtr, bool rts) { modem->onLineState(dtr, rts); }
  void pcWrite(const std::vector<uint8_t>& b) { modem->onHostBytes(b.data(), b.size()); }
  void radioRx(const std::vector<uint8_t>& b) { modem->onUartRx(b.data(), b.size()); }

  // Plays a host program that keys by RTS the way the real HSDU was driven: RTS up, 80 ms, write the whole
  // wire image, wait its airtime plus 30 ms, hold a further 1 ms per byte, RTS down.
  void lqSend(const std::vector<uint8_t>& wire, uint32_t baud = 4800) {
    lines(true, true);
    run(80000);
    pcWrite(wire);
    const uint32_t airUs = static_cast<uint32_t>(wire.size()) * 10000000u / baud;
    run(airUs + 30000);
    run(static_cast<uint32_t>(wire.size()) * 1000u);
    lines(true, false);
    run(10000);  // the firmware holds the carrier ~3 ms after the last bit, then listens again
  }

  // What a host writes for one transmission: the preamble run of 0xAA, then the payload. The modem never looks
  // inside, so the payload here is arbitrary.
  static std::vector<uint8_t> wireImage(const std::vector<uint8_t>& payload, int preambleBytes = 9) {
    std::vector<uint8_t> w(static_cast<size_t>(preambleBytes), 0xAA);
    w.insert(w.end(), payload.begin(), payload.end());
    return w;
  }

  // First time the chip was in TX at or after t0, and the time it left TX afterwards (0 if still in TX).
  bool firstTxWindow(uint32_t t0, uint32_t* on, uint32_t* off) const {
    bool inTx = false;
    for (const auto& c : chip.history) {
      if (static_cast<int32_t>(c.t - t0) < 0) continue;
      if (!inTx && c.marc == openhsdu::cc1101::kMarcTx) {
        inTx = true;
        *on = c.t;
        *off = 0;
      } else if (inTx && c.marc != openhsdu::cc1101::kMarcTx) {
        *off = c.t;
        return true;
      }
    }
    return inTx;
  }

  int txWindows() const {
    int n = 0;
    uint8_t prev = openhsdu::cc1101::kMarcIdle;
    for (const auto& c : chip.history) {
      if (c.marc == openhsdu::cc1101::kMarcTx && prev != openhsdu::cc1101::kMarcTx) ++n;
      prev = c.marc;
    }
    return n;
  }

  // True if no byte began to be sent after the carrier was dropped: each began while the chip was transmitting, or in the
  // very microsecond it was cut (a byte queued back-to-back behind another starts exactly when the last one ends, so a cut
  // at that instant cannot tell "just started" from "about to start").
  bool allBytesStartedInTx() const {
    for (const auto& s : uart.sent)
      if (!chip.transmittingAt(s.startUs) && !chip.transmittingAt(s.startUs - 1)) return false;
    return true;
  }

  // True if every byte the UART put on the wire was sent while the chip was transmitting, and (with a latency)
  // the carrier was still up that long after the byte's last bit, as the chip's modulator needs (8 bit periods).
  bool allBytesSentInTx(uint32_t latencyUs = 0) const {
    for (const auto& s : uart.sent) {
      if (!chip.transmittingAt(s.startUs) || !chip.transmittingAt(s.endUs + latencyUs - 1)) return false;
    }
    return true;
  }
};

}  // namespace sim
