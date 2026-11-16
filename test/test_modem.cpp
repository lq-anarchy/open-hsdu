// The modem's behaviour, one situation per test. The chip model records every state change, the fake UART
// records every byte with its start and end time on the wire, and the host records every byte it was sent.
#include <algorithm>
#include <numeric>

#include "bench.h"
#include "doctest.h"

using namespace openhsdu;
using sim::Bench;

namespace {

const std::vector<uint8_t> kFrame = {0x31, 0x32, 0x33, 0x34, 0x35};  // opaque bytes: the modem never looks inside

ModemConfig rtsMode() {
  ModemConfig c;
  c.keyMode = KeyMode::kRts;
  return c;
}
ModemConfig dataMode() {
  ModemConfig c;
  c.keyMode = KeyMode::kData;
  return c;
}

std::vector<uint8_t> sequence(size_t n, uint8_t start = 0) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(start + i);
  return v;
}

// Runs until the modem has returned to receive; fails the test if it does not within `limitUs`.
void runUntilRx(Bench& b, uint32_t limitUs = 5000000) {
  uint32_t used = 0;
  while (b.modem->keyed() && used < limitUs) {
    b.run(100);
    used += 100;
  }
  REQUIRE_FALSE(b.modem->keyed());
}

}  // namespace

TEST_SUITE("modem") {

TEST_CASE("boot: the radio is up, receiving, and the modem is idle") {
  Bench b;
  REQUIRE(b.boot());
  CHECK(b.device->present());
  CHECK(b.chip.marc() == cc1101::kMarcRx);
  CHECK(b.modem->state() == Modem::State::kRx);
  CHECK_FALSE(b.modem->keyed());
}

// ---------------------------------------------------------------------------------------------------------
// Keyed by RTS, the way a host program drives the real HSDU.

TEST_CASE("RTS keying: a frame goes out intact, once, with the carrier up before the first byte and echoed to the PC") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, false);
  b.run(5000);
  const auto wire = Bench::wireImage(kFrame);
  const uint32_t t0 = b.clock.micros();
  b.lqSend(wire);

  CHECK(b.uart.bytes() == wire);
  CHECK(b.host.received == wire);  // the echo
  CHECK(b.allBytesSentInTx());
  CHECK(b.txWindows() == 1);

  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(t0, &on, &off));
  REQUIRE(off != 0);
  const uint32_t lead = b.uart.sent.front().startUs - on;
  CHECK(lead >= 78000);  // the PC's 80 ms lead, less the chip's calibration
  CHECK(lead <= 82000);
  const uint32_t tail = off - b.uart.sent.back().endUs;
  CHECK(tail >= 45000);  // the PC holds 30 ms plus 1 ms per byte after the airtime
  CHECK(tail <= 55000);

  CHECK(b.modem->state() == Modem::State::kRx);
  CHECK(b.chip.marc() == cc1101::kMarcRx);
  CHECK(b.modem->stats().keyUps == 1);
  CHECK(b.modem->stats().watchdogTrips == 0);
  CHECK(b.modem->stats().radioFaults == 0);
}

TEST_CASE("RTS keying: the first byte waits for the PLL to settle even if the PC writes at once") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.pcWrite(kFrame);
  b.run(100000);
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  REQUIRE(b.uart.sent.size() == kFrame.size());
  CHECK(b.uart.sent.front().startUs - on >= b.modem->config().radioSettleUs);
  CHECK(b.allBytesSentInTx());
}

TEST_CASE("RTS keying: a frame sent without RTS is ignored and counted") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, false);
  b.pcWrite(kFrame);
  b.run(500000);
  CHECK(b.uart.sent.empty());
  CHECK(b.txWindows() == 0);
  CHECK(b.modem->stats().txDroppedNoKey == kFrame.size());
}

TEST_CASE("RTS keying: letting go of RTS right after the write does not cut the frame short") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(5000);
  const auto data = sequence(60, 0x40);
  b.pcWrite(data);
  b.lines(true, false);  // the PC dropped RTS at once; the bytes are still being clocked out
  b.run(300000);
  CHECK(b.uart.bytes() == data);
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  REQUIRE(off != 0);
  CHECK(off >= b.uart.sent.back().endUs);  // the carrier outlived the last bit
  CHECK(b.allBytesSentInTx());
}

TEST_CASE("RTS keying: a frame longer than the UART's queue is fed in as space opens, and still ends cleanly") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(5000);
  const auto data = sequence(300, 0x10);  // the fake UART holds 128
  b.pcWrite(data);
  b.lines(true, false);
  b.run(1000000);
  CHECK(b.uart.bytes() == data);
  CHECK(b.host.received == data);
  CHECK(b.allBytesSentInTx());
  CHECK_FALSE(b.modem->keyed());
}

TEST_CASE("RTS keying: RTS held with nothing to send keeps the carrier up until the PC lets go") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(500000);
  CHECK(b.modem->keyed());
  CHECK(b.chip.marc() == cc1101::kMarcTx);
  b.lines(true, false);
  b.run(10000);
  CHECK_FALSE(b.modem->keyed());
  CHECK(b.chip.marc() == cc1101::kMarcRx);
}

TEST_CASE("RTS keying: echo can be turned off") {
  ModemConfig c = rtsMode();
  c.echo = false;
  Bench b(c);
  REQUIRE(b.boot());
  b.lines(true, false);
  b.lqSend(Bench::wireImage(kFrame));
  CHECK(b.host.received.empty());
  CHECK(b.uart.bytes() == Bench::wireImage(kFrame));
}

// ---------------------------------------------------------------------------------------------------------
// Keyed by data: the firmware adds the lead-in and tail the PC would otherwise supply.

TEST_CASE("data keying: bytes alone key the radio, with the HSDU's lead-in and tail") {
  Bench b(dataMode());
  REQUIRE(b.boot());
  b.lines(true, false);
  b.run(5000);
  const auto wire = Bench::wireImage(kFrame);
  const uint32_t t0 = b.clock.micros();
  b.pcWrite(wire);
  b.run(1000000);

  CHECK(b.uart.bytes() == wire);
  CHECK(b.host.received == wire);
  CHECK(b.allBytesSentInTx());
  CHECK(b.txWindows() == 1);
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(t0, &on, &off));
  REQUIRE(off != 0);
  const uint32_t lead = b.uart.sent.front().startUs - on;
  CHECK(lead >= 85000);
  CHECK(lead <= 86000);
  const uint32_t tail = off - b.uart.sent.back().endUs;
  CHECK(tail >= 80000);
  CHECK(tail <= 81000);
  CHECK_FALSE(b.modem->keyed());
}

TEST_CASE("data keying: RTS is ignored") {
  Bench b(dataMode());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(500000);
  CHECK_FALSE(b.modem->keyed());
  CHECK(b.txWindows() == 0);
}

TEST_CASE("data keying: more bytes during the tail continue the same transmission without a second lead-in") {
  Bench b(dataMode());
  REQUIRE(b.boot());
  b.lines(true, false);
  b.pcWrite(sequence(5, 0x20));
  b.run(125000);  // 85 ms lead + 5 bytes (10.4 ms) + 29 ms into the 80 ms tail
  REQUIRE(b.modem->state() == Modem::State::kTxTail);
  b.pcWrite(sequence(5, 0x60));
  b.run(300000);
  CHECK(b.txWindows() == 1);
  REQUIRE(b.uart.sent.size() == 10);
  const uint32_t gap = b.uart.sent[5].startUs - b.uart.sent[4].endUs;
  CHECK(gap < 30000);  // no second 85 ms lead-in
  CHECK(b.allBytesSentInTx());
}

TEST_CASE("data keying: the lead-in is also the minimum before the first byte of a short burst") {
  Bench b(dataMode());
  REQUIRE(b.boot());
  b.lines(true, false);
  b.pcWrite({0x55});
  b.run(400000);
  REQUIRE(b.uart.sent.size() == 1);
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  CHECK(b.uart.sent[0].startUs - on >= 85000);
}

// ---------------------------------------------------------------------------------------------------------
// Either: RTS or data.

TEST_CASE("either: RTS-keyed frames are not given a second lead-in by the firmware") {
  Bench b;  // default key mode is either
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(10000);
  b.pcWrite(kFrame);
  b.run(100000);
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  REQUIRE(b.uart.sent.size() == kFrame.size());
  CHECK(b.uart.sent.front().startUs - on < 12000);  // about the 10 ms the PC waited
}

TEST_CASE("either: bytes without RTS still go out, with the firmware's own lead-in and tail") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  b.pcWrite(kFrame);
  b.run(500000);
  CHECK(b.uart.bytes() == kFrame);
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  CHECK(b.uart.sent.front().startUs - on >= 85000);
}

TEST_CASE("either: RTS raised during a data-keyed burst holds the carrier up after the bytes are done") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  b.pcWrite(kFrame);
  b.run(120000);  // in the tail
  b.lines(true, true);
  b.run(500000);
  CHECK(b.modem->keyed());  // the PC is holding RTS
  b.lines(true, false);
  b.run(300000);
  CHECK_FALSE(b.modem->keyed());
  CHECK(b.txWindows() == 1);
}

// ---------------------------------------------------------------------------------------------------------
// Receive.

TEST_CASE("receive: bytes from the radio reach the PC unchanged and in order") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  b.run(10000);
  const auto data = sequence(40, 0x80);
  b.radioRx(data);
  b.run(1000);
  CHECK(b.host.received == data);
  CHECK(b.modem->stats().rxBytes == 40);
}

TEST_CASE("receive: radio bytes seen while transmitting are discarded, not forwarded") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(5000);
  b.radioRx(sequence(20, 0x80));
  b.run(1000);
  CHECK(b.host.received.empty());
  CHECK(b.modem->stats().rxDiscardedTx == 20);
}

TEST_CASE("receive: bytes in the first 3 ms after a transmission are discarded (the chip is still switching)") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(5000);
  b.lines(true, false);
  while (b.modem->keyed()) b.run(100);
  // The tick that returned to receive ends here, and the settle time counts from this instant.
  b.clock.advance(2999);
  b.radioRx({0x90});
  CHECK(b.modem->stats().rxDiscardedSettle == 1);
  b.clock.advance(1);  // exactly 3000 us after
  b.radioRx({0x91});
  b.run(100);
  CHECK(b.host.received == std::vector<uint8_t>{0x91});
}

TEST_CASE("receive: the UART's receive queue is flushed when the transmitter stops") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(5000);
  const int before = b.uart.flushRxCalls;
  b.lines(true, false);
  b.run(10000);
  CHECK(b.uart.flushRxCalls == before + 1);
}

TEST_CASE("receive: nothing is delivered if the PC has not opened the port, and nothing stale after it does") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(false, false);
  b.run(10000);
  b.radioRx(sequence(10, 0x80));
  b.run(1000);
  CHECK(b.host.received.empty());
  CHECK(b.modem->stats().hostDroppedNoDtr == 10);
  b.lines(true, false);
  b.run(1000);
  CHECK(b.host.received.empty());
}

TEST_CASE("receive: closing the port clears what was queued for it") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  b.run(10000);
  b.host.blocked = true;  // USB side full
  b.radioRx(sequence(30, 0x80));
  b.run(1000);
  b.lines(false, false);
  b.host.blocked = false;
  b.lines(true, false);
  b.run(5000);
  CHECK(b.host.received.empty());
}

TEST_CASE("receive: a USB side that accepts only a few bytes at a time still gets everything, in order") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  b.run(10000);
  b.host.maxPerWrite = 3;
  const auto data = sequence(100, 0x80);
  b.radioRx(data);
  b.run(20000);
  CHECK(b.host.received == data);
}

TEST_CASE("receive: when the PC-bound buffer fills the newest bytes are dropped and counted, the oldest are kept") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  b.run(10000);
  b.host.blocked = true;
  std::vector<uint8_t> big(3000);
  for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 7);
  b.radioRx(big);
  b.run(1000);
  CHECK(b.modem->stats().hostDroppedFull == 3000 - Modem::kHostBufferSize);
  b.host.blocked = false;
  b.run(2000);
  REQUIRE(b.host.received.size() == Modem::kHostBufferSize);
  CHECK(std::equal(b.host.received.begin(), b.host.received.end(), big.begin()));
}

// ---------------------------------------------------------------------------------------------------------
// Protection.

TEST_CASE("watchdog: RTS stuck on is cut at the limit and cannot re-key until the PC lets go once") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(3000000);
  CHECK_FALSE(b.modem->keyed());
  CHECK(b.modem->lockedOut());
  CHECK(b.modem->stats().watchdogTrips == 1);
  CHECK(b.txWindows() == 1);
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  REQUIRE(off != 0);
  CHECK(off - on >= 2000000);
  CHECK(off - on <= 2010000);
  // still holding RTS: bytes are refused, no new carrier
  b.pcWrite(kFrame);
  b.run(500000);
  CHECK(b.txWindows() == 1);
  CHECK(b.modem->stats().txDroppedNoKey == kFrame.size());
  // let go, press again: works
  b.lines(true, false);
  CHECK_FALSE(b.modem->lockedOut());
  b.lines(true, true);
  b.run(50000);
  CHECK(b.modem->keyed());
  CHECK(b.txWindows() == 2);
}

TEST_CASE("watchdog: a flood of data is cut at the limit and the rest is counted as dropped") {
  Bench b(dataMode());
  REQUIRE(b.boot());
  b.lines(true, false);
  b.pcWrite(sequence(1000, 0));  // 2.08 s of airtime plus the lead-in: more than the 2 s limit
  b.run(3000000);
  CHECK(b.modem->stats().watchdogTrips == 1);
  const Modem::Stats& s = b.modem->stats();
  CHECK(s.txDroppedFault + s.txAborted > 0);
  CHECK_FALSE(b.modem->keyed());
  CHECK(b.chip.marc() == cc1101::kMarcRx);
  CHECK(s.txBytes + s.txDroppedFault == 1000);  // handed to the UART + never handed over = everything the PC wrote
  CHECK(b.uart.sent.size() == s.txBytes - s.txAborted);
  CHECK(b.allBytesStartedInTx());  // (a byte in the shift register when the carrier is cut finishes after it)
}

TEST_CASE("watchdog: the limit is configurable") {
  ModemConfig c = rtsMode();
  c.maxTxUs = 300000;
  Bench b(c);
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(1000000);
  CHECK(b.modem->stats().watchdogTrips == 1);
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  CHECK(off - on <= 310000);
}

TEST_CASE("transmit buffer: bytes beyond its size are dropped and counted, not wrapped over the ones stored") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, true);
  const auto big = sequence(2000, 0);
  b.pcWrite(big);
  CHECK(b.modem->stats().txDroppedFull == 2000 - Modem::kTxBufferSize);
  b.run(1000);  // first ticks hand bytes to the UART
  CHECK(b.uart.bytes().size() <= 128);
  CHECK(std::equal(b.uart.bytes().begin(), b.uart.bytes().end(), big.begin()));
}

TEST_CASE("radio fault: if the chip will not transmit, nothing is sent, the bytes are counted, and it backs off") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.chip.loseTx = true;
  b.pcWrite(kFrame);
  b.run(1000);
  CHECK(b.modem->stats().radioFaults >= 1);
  const uint32_t first = b.modem->stats().keyUpFailures;
  CHECK(b.modem->stats().keyUps == 0);
  CHECK(b.modem->stats().txDroppedFault == kFrame.size());
  CHECK(b.uart.sent.empty());
  b.run(50000);  // inside the 100 ms back-off: no new key-up attempt
  CHECK(b.modem->stats().keyUpFailures == first);
  b.run(200000);  // past it: tries again, fails again
  CHECK(b.modem->stats().keyUpFailures > first);
  b.chip.loseTx = false;
  b.run(200000);  // and now it works, with RTS still held
  CHECK(b.modem->keyed());
  CHECK(b.modem->stats().keyUps == 1);
}

TEST_CASE("radio fault: a chip that is slow to leave transmit is retried until it is back in receive") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(5000);
  REQUIRE(b.modem->keyed());
  b.chip.ignoreNextStrobes = 1;  // the SIDLE that ends the transmission is lost
  b.lines(true, false);
  b.run(20000);
  CHECK(b.modem->stats().radioFaults >= 1);
  CHECK_FALSE(b.modem->keyed());
  CHECK(b.chip.marc() == cc1101::kMarcRx);
}

TEST_CASE("clock wrap-around: a whole exchange across the 32-bit boundary behaves the same") {
  Bench b(rtsMode(), 0xFFFFF000u);
  REQUIRE(b.boot());
  b.lines(true, false);
  b.run(5000);
  const auto wire = Bench::wireImage(kFrame);
  b.lqSend(wire);
  CHECK(b.uart.bytes() == wire);
  CHECK(b.host.received == wire);
  CHECK(b.modem->stats().watchdogTrips == 0);
  CHECK(b.clock.micros() < 0xFFFFF000u);  // it really did wrap
}

TEST_CASE("clock wrap-around: data keying across the boundary") {
  Bench b(dataMode(), 0xFFFF0000u);
  REQUIRE(b.boot());
  b.lines(true, false);
  b.pcWrite(kFrame);
  b.run(500000);
  CHECK(b.uart.bytes() == kFrame);
  CHECK(b.modem->stats().watchdogTrips == 0);
  CHECK_FALSE(b.modem->keyed());
}

// ---------------------------------------------------------------------------------------------------------
// Bench helpers: a plain carrier and a run of 0xAA.

TEST_CASE("carrier burst: unmodulated, for about the time asked, no bytes on the wire") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  REQUIRE(b.modem->carrierBurst(200000));
  b.run(20000);
  CHECK(b.modem->keyed());
  CHECK_FALSE(b.modem->carrierBurst(100000));  // already busy
  runUntilRx(b);
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  REQUIRE(off != 0);
  CHECK(off - on >= 200000);
  CHECK(off - on <= 300000);  // the burst plus the 80 ms tail
  CHECK(b.uart.sent.empty());
}

TEST_CASE("carrier burst: refused for zero length, and a huge request is capped well under the watchdog") {
  Bench b;
  REQUIRE(b.boot());
  CHECK_FALSE(b.modem->carrierBurst(0));
  REQUIRE(b.modem->carrierBurst(60000000));
  b.run(2000);
  REQUIRE(b.modem->keyed());
  runUntilRx(b, 4000000);
  CHECK(b.modem->stats().watchdogTrips == 0);
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  CHECK(off - on < 1200000);
}

TEST_CASE("sendBytes: works in RTS mode too, with a lead-in, and is echoed") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, false);
  const std::vector<uint8_t> aa(8, 0xAA);
  REQUIRE(b.modem->sendBytes(aa.data(), aa.size()));
  b.run(500000);
  CHECK(b.uart.bytes() == aa);
  CHECK(b.host.received == aa);
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  CHECK(b.uart.sent.front().startUs - on >= 85000);
  CHECK(b.allBytesSentInTx());
}

TEST_CASE("sendBytes refuses while keyed or with nothing to send") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(5000);
  const uint8_t x = 1;
  CHECK_FALSE(b.modem->sendBytes(&x, 1));
  b.lines(true, false);
  b.run(5000);
  CHECK_FALSE(b.modem->sendBytes(&x, 0));
}

}  // TEST_SUITE
