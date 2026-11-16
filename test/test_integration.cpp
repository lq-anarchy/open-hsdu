// End to end, in software: a PC that keys by RTS the way a host program drives the real HSDU, a device built from the
// modem, driver and chip model, and packs that answer. These are the claims docs/THEORY.md makes about the
// link, run as a script.
#include <numeric>

#include "bench.h"
#include "doctest.h"

using namespace openhsdu;
using sim::Bench;

namespace {

// What a pack's reply looks like at the UART: a run of 0xAA, then the frame. Opaque bytes here.
std::vector<uint8_t> replyImage(const std::vector<uint8_t>& frame) {
  std::vector<uint8_t> r(9, 0xAA);
  r.insert(r.end(), frame.begin(), frame.end());
  return r;
}

}  // namespace

TEST_SUITE("integration") {

TEST_CASE("twenty request/reply exchanges: every frame out once, every reply in once, in order, nothing lost") {
  ModemConfig cfg;
  cfg.keyMode = KeyMode::kRts;
  Bench b(cfg);
  REQUIRE(b.boot());
  b.lines(true, false);
  b.run(10000);

  std::vector<uint8_t> expectedHost, expectedAir;
  for (int i = 0; i < 20; ++i) {
    std::vector<uint8_t> frame = {0x7B, static_cast<uint8_t>('A' + i), static_cast<uint8_t>(i), 0x7D};
    const auto wire = Bench::wireImage(frame);
    b.lqSend(wire);
    expectedAir.insert(expectedAir.end(), wire.begin(), wire.end());
    expectedHost.insert(expectedHost.end(), wire.begin(), wire.end());  // the echo

    b.run(20000);  // a pack takes a moment to answer
    const auto reply = replyImage({0x7B, static_cast<uint8_t>('a' + i), static_cast<uint8_t>(0x80 + i), 0x7D});
    b.radioRx(reply);
    b.run(2000);
    expectedHost.insert(expectedHost.end(), reply.begin(), reply.end());
  }

  CHECK(b.uart.bytes() == expectedAir);
  CHECK(b.host.received == expectedHost);
  CHECK(b.allBytesSentInTx());
  CHECK(b.txWindows() == 20);
  CHECK(b.modem->stats().keyUps == 20);
  CHECK(b.modem->stats().watchdogTrips == 0);
  CHECK(b.modem->stats().radioFaults == 0);
  CHECK(b.modem->stats().rxDiscardedSettle == 0);
}

TEST_CASE("turnaround: the radio is listening again within 5 ms of the PC releasing RTS, far inside the ~180 ms the packs allow") {
  ModemConfig cfg;
  cfg.keyMode = KeyMode::kRts;
  Bench b(cfg);
  REQUIRE(b.boot());
  b.lines(true, false);
  b.run(10000);
  const auto wire = Bench::wireImage({0x7B, 0x41, 0x7D});
  b.lines(true, true);
  b.run(80000);
  b.pcWrite(wire);
  b.run(static_cast<uint32_t>(wire.size()) * 2083u + 60000u);
  const uint32_t release = b.clock.micros();
  b.lines(true, false);
  while (b.modem->keyed()) b.run(50);
  b.run(5000);
  REQUIRE(b.chip.marc() == cc1101::kMarcRx);
  // when did the chip enter RX after the release?
  uint32_t rxAt = 0;
  for (const auto& c : b.chip.history)
    if (static_cast<int32_t>(c.t - release) >= 0 && c.marc == cc1101::kMarcRx) {
      rxAt = c.t;
      break;
    }
  REQUIRE(rxAt != 0);
  CHECK(rxAt - release >= 3000);  // the carrier is held 3 ms for the chip's own transmit latency
  CHECK(rxAt - release < 5000);
  // and from the last bit on the air, including the PC's own hold time, it is under the packs' limit
  CHECK(rxAt - b.uart.sent.back().endUs < 150000);
}

TEST_CASE("the same turnaround holds when the firmware supplies the tail (data keying)") {
  ModemConfig cfg;
  cfg.keyMode = KeyMode::kData;
  Bench b(cfg);
  REQUIRE(b.boot());
  b.lines(true, false);
  b.pcWrite(Bench::wireImage({0x7B, 0x41, 0x7D}));
  b.run(400000);
  REQUIRE(b.chip.marc() == cc1101::kMarcRx);
  uint32_t rxAt = 0;
  for (const auto& c : b.chip.history)
    if (c.marc == cc1101::kMarcRx) rxAt = c.t;
  CHECK(rxAt - b.uart.sent.back().endUs < 150000);
}

TEST_CASE("a reply that arrives right after the carrier drops is not lost") {
  // The pack answers about 20 ms after the end of the base's frame in the field; here it starts a few ms
  // after the PC lets go of RTS, once the receiver has settled.
  ModemConfig cfg;
  cfg.keyMode = KeyMode::kRts;
  Bench b(cfg);
  REQUIRE(b.boot());
  b.lines(true, false);
  b.lqSend(Bench::wireImage({0x7B, 0x41, 0x7D}));
  b.host.received.clear();
  b.run(3500);  // a few ms after the carrier came down: past the 3 ms the chip needs
  const auto reply = replyImage({0x7B, 0x61, 0x7D});
  b.radioRx(reply);
  b.run(1000);
  CHECK(b.host.received == reply);
}

TEST_CASE("the receive path has no squelch: noise bytes flow, like the HSDU, and the PC filters them") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  b.run(10000);
  std::vector<uint8_t> noise(500);
  for (size_t i = 0; i < noise.size(); ++i) noise[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
  for (size_t i = 0; i < noise.size(); i += 50) {
    b.radioRx(std::vector<uint8_t>(noise.begin() + i, noise.begin() + i + 50));
    b.run(1000);
  }
  CHECK(b.host.received == noise);
}

TEST_CASE("switching key modes at run time takes effect on the next transmission") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  b.modem->setKeyMode(KeyMode::kRts);
  b.pcWrite({1, 2, 3});
  b.run(300000);
  CHECK(b.uart.sent.empty());  // no RTS, RTS mode: ignored
  b.modem->setKeyMode(KeyMode::kData);
  b.pcWrite({4, 5, 6});
  b.run(500000);
  CHECK(b.uart.bytes() == std::vector<uint8_t>{4, 5, 6});
}

TEST_CASE("a power cycle in the middle of a transmission is recovered by the next one") {
  ModemConfig cfg;
  cfg.keyMode = KeyMode::kRts;
  Bench b(cfg);
  REQUIRE(b.boot());
  b.lines(true, true);
  b.pcWrite(Bench::wireImage({1, 2, 3}));
  b.run(20000);  // mid-frame
  REQUIRE(b.modem->keyed());
  b.lines(false, false);  // the USB cable came out: DTR and RTS both drop
  b.run(300000);
  CHECK_FALSE(b.modem->keyed());  // the bytes already queued finish, then the carrier drops
  CHECK(b.chip.marc() == cc1101::kMarcRx);
  b.lines(true, false);
  b.run(5000);
  b.lqSend(Bench::wireImage({7, 8, 9}));
  CHECK(b.host.received.size() >= 3);
  CHECK(b.modem->stats().watchdogTrips == 0);
}

}  // TEST_SUITE
