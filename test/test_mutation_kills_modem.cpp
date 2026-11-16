// Tests that kill the modem mutants the existing suite lets through. Each one passes on the unmodified code.
#include <algorithm>
#include <vector>

#include "bench.h"
#include "doctest.h"

using namespace openhsdu;
using sim::Bench;

namespace {

const std::vector<uint8_t> kFrameK = {0x31, 0x32, 0x33, 0x34, 0x35};

ModemConfig rtsModeK() {
  ModemConfig c;
  c.keyMode = KeyMode::kRts;
  return c;
}

std::vector<uint8_t> seqK(size_t n, uint8_t start = 0) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(start + i);
  return v;
}

// One tick at an exact clock value (no stepping), so a boundary can be hit to the microsecond.
void tickAt(Bench& b, uint32_t t) {
  const uint32_t now = b.clock.micros();
  REQUIRE(static_cast<int32_t>(t - now) >= 0);
  b.clock.advance(t - now);
  b.modem->tick();
}

}  // namespace

TEST_SUITE("mutation-kills") {

// ---- DTR -------------------------------------------------------------------------------------------------

TEST_CASE("DTR: a control-line change while the port stays open does not discard bytes queued for the PC") {
  Bench b(rtsModeK());
  REQUIRE(b.boot());
  b.lines(true, false);
  b.run(10000);
  b.host.blocked = true;  // USB side full: the bytes wait in the PC-bound buffer
  const auto data = seqK(30, 0x80);
  b.radioRx(data);
  b.run(1000);
  b.lines(true, true);  // RTS toggles, DTR never drops
  b.lines(true, false);
  b.host.blocked = false;
  b.run(5000);
  CHECK(b.host.received == data);
}

// ---- host bytes while the carrier is up -----------------------------------------------------------------

TEST_CASE("RTS keying: bytes that arrive after RTS was released but while the carrier is still up are sent, not dropped") {
  Bench b(rtsModeK());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(5000);
  REQUIRE(b.modem->keyed());
  const auto first = seqK(60, 0x40);
  const auto second = seqK(10, 0x20);
  b.pcWrite(first);
  b.lines(true, false);  // the control-line event overtakes the last USB packet
  b.pcWrite(second);     // ... which arrives while the radio is still transmitting
  b.run(400000);
  auto both = first;
  both.insert(both.end(), second.begin(), second.end());
  CHECK(b.uart.bytes() == both);
  CHECK(b.modem->stats().txDroppedNoKey == 0);
}

// ---- stale bytes: RTS withdrawn before the key-up -------------------------------------------------------

TEST_CASE("RTS keying: bytes written while RTS was up but withdrawn before key-up never reach the next transmission") {
  Bench b(rtsModeK());
  REQUIRE(b.boot());
  b.lines(true, false);
  b.run(10000);
  b.lines(true, true);
  b.pcWrite(kFrameK);     // accepted: RTS is up ...
  b.lines(true, false);   // ... but the PC lets go before the first tick can key the radio
  b.run(20000);
  CHECK(b.modem->stats().txDroppedNoKey == kFrameK.size());  // counted once, not once per tick
  b.lines(true, true);    // the next request
  b.run(300000);
  CHECK(b.uart.sent.empty());  // nothing from the withdrawn request
}

TEST_CASE("sendBytes: a finished bench transmission does not leave RTS mode open to unkeyed bytes") {
  Bench b(rtsModeK());
  REQUIRE(b.boot());
  b.lines(true, false);
  const std::vector<uint8_t> aa(4, 0xAA);
  REQUIRE(b.modem->sendBytes(aa.data(), aa.size()));
  b.run(500000);
  REQUIRE_FALSE(b.modem->keyed());
  REQUIRE(b.uart.bytes() == aa);
  b.lines(true, true);
  b.pcWrite(kFrameK);
  b.lines(true, false);  // withdrawn before it could key
  b.run(300000);
  CHECK(b.uart.bytes() == aa);  // the frame must not have been transmitted without RTS
}

TEST_CASE("sendBytes: a bench transmission the chip refused does not leave RTS mode open to unkeyed bytes") {
  Bench b(rtsModeK());
  REQUIRE(b.boot());
  b.lines(true, false);
  b.chip.freezeState = true;
  const std::vector<uint8_t> aa(4, 0xAA);
  REQUIRE(b.modem->sendBytes(aa.data(), aa.size()));
  b.run(1000);
  REQUIRE(b.modem->stats().radioFaults >= 1);
  b.chip.freezeState = false;
  b.run(300000);  // past the back-off
  b.lines(true, true);
  b.pcWrite(kFrameK);
  b.lines(true, false);
  b.run(300000);
  CHECK(b.uart.sent.empty());
}

TEST_CASE("sendBytes reports failure when the transmit buffer cannot hold everything") {
  Bench b(rtsModeK());
  REQUIRE(b.boot());
  const auto big = seqK(2000, 0);
  CHECK_FALSE(b.modem->sendBytes(big.data(), big.size()));
  CHECK(b.modem->stats().txDroppedFull == big.size() - Modem::kTxBufferSize);
}

// ---- radio faults -----------------------------------------------------------------------------------------

TEST_CASE("radio fault: bytes queued when the chip would not key are discarded, not sent when it recovers") {
  Bench b(rtsModeK());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.chip.freezeState = true;
  b.pcWrite(kFrameK);
  b.run(1000);
  REQUIRE(b.modem->stats().radioFaults >= 1);
  b.chip.freezeState = false;
  b.run(300000);  // past the back-off, RTS still held
  REQUIRE(b.modem->keyed());
  b.lines(true, false);
  b.run(20000);
  CHECK(b.uart.sent.empty());
  CHECK(b.modem->stats().txDroppedFault == kFrameK.size());
}

TEST_CASE("radio fault: a key-up the chip was too slow to complete is cancelled and never comes up as a carrier") {
  Bench b(rtsModeK());
  REQUIRE(b.boot());
  b.chip.calUs = 20000;  // calibration takes longer than the driver waits for it (6 ms)
  b.lines(true, true);
  b.run(400000);
  CHECK(b.modem->stats().radioFaults >= 1);
  CHECK(b.modem->stats().keyUps == 0);
  CHECK(b.txWindows() == 0);  // the pending TX was cancelled by the SIDLE that follows the fault
}

TEST_CASE("receive: the PC-bound buffer holds a full 2048 bytes") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  b.run(10000);
  b.host.blocked = true;
  b.radioRx(seqK(2048, 0));
  b.run(1000);
  CHECK(b.modem->stats().hostDroppedFull == 0);
  b.radioRx({0x90});
  CHECK(b.modem->stats().hostDroppedFull == 1);
}

TEST_CASE("radio fault: a refused bench carrier is abandoned, not resumed when the chip recovers") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  b.chip.freezeState = true;
  REQUIRE(b.modem->carrierBurst(600000));
  b.run(1000);
  REQUIRE(b.modem->stats().radioFaults >= 1);
  b.chip.freezeState = false;
  b.run(250000);  // past the back-off and well inside the 600 ms burst that was asked for
  CHECK_FALSE(b.modem->keyed());
  CHECK(b.modem->stats().keyUps == 0);
}

TEST_CASE("radio fault: the retry happens at the back-off limit, not a tick before and not a tick after") {
  Bench b(rtsModeK());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.chip.loseTx = true;
  b.pcWrite(kFrameK);
  b.clock.advance(100);
  b.modem->tick();  // first failure; the back-off starts at the clock value this tick ends on
  const uint32_t t0 = b.clock.micros();
  const uint32_t faults = b.modem->stats().keyUpFailures;
  REQUIRE(faults == 1);
  tickAt(b, t0 + b.modem->config().faultBackoffUs - 1);
  CHECK(b.modem->stats().keyUpFailures == faults);  // still backing off
  tickAt(b, t0 + b.modem->config().faultBackoffUs);
  CHECK(b.modem->stats().keyUpFailures == faults + 1);  // exactly at the limit it tries again
}

TEST_CASE("radio fault: the back-off is 100 ms") {
  Bench b(rtsModeK());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.chip.loseTx = true;
  b.pcWrite(kFrameK);
  b.clock.advance(100);
  b.modem->tick();
  const uint32_t t0 = b.clock.micros();
  const uint32_t faults = b.modem->stats().keyUpFailures;
  tickAt(b, t0 + 99000);
  CHECK(b.modem->stats().keyUpFailures == faults);
  tickAt(b, t0 + 101000);
  CHECK(b.modem->stats().keyUpFailures == faults + 1);
}

// ---- clock wrap: stale timers must not come back to life after 2^31 us (35.8 minutes) -------------------

TEST_CASE("clock wrap: a finished bench carrier does not key the radio again 36 minutes later") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  REQUIRE(b.modem->carrierBurst(100000));
  b.run(400000);
  REQUIRE_FALSE(b.modem->keyed());
  REQUIRE(b.modem->stats().keyUps == 1);
  b.clock.advance(0x80001000u);  // the board sat idle for 35.8 minutes
  b.modem->tick();
  b.run(1000);
  CHECK_FALSE(b.modem->keyed());
  CHECK(b.modem->stats().keyUps == 1);
}

TEST_CASE("clock wrap: a recovered radio fault does not block keying 36 minutes later") {
  Bench b(rtsModeK());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.chip.freezeState = true;
  b.run(1000);
  REQUIRE(b.modem->stats().radioFaults >= 1);
  b.chip.freezeState = false;
  b.run(300000);
  REQUIRE(b.modem->keyed());
  b.lines(true, false);
  b.run(20000);
  REQUIRE_FALSE(b.modem->keyed());
  b.clock.advance(0x80001000u);
  b.lines(true, true);
  b.run(50000);
  CHECK(b.modem->keyed());
}

TEST_CASE("test carrier: stops at the requested time to the microsecond") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  const uint32_t t0 = b.clock.micros();
  REQUIRE(b.modem->carrierBurst(200000));
  const uint32_t until = t0 + 200000;
  while (b.clock.micros() + 100 < until) b.run(100);
  REQUIRE(b.modem->state() == Modem::State::kTxData);
  tickAt(b, until - 1);
  CHECK(b.modem->state() == Modem::State::kTxData);  // one microsecond early: still carrying
  tickAt(b, until);
  CHECK(b.modem->state() == Modem::State::kTxTail);  // at the limit: the carrier is over
}

TEST_CASE("test carrier: a request for a very long carrier is cut to half the watchdog limit") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  REQUIRE(b.modem->carrierBurst(60000000));
  b.run(2000);
  REQUIRE(b.modem->keyed());
  uint32_t used = 0;
  while (b.modem->keyed() && used < 4000000) {
    b.run(100);
    used += 100;
  }
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  REQUIRE(off != 0);
  const uint32_t half = b.modem->config().maxTxUs / 2;
  CHECK(off - on >= half);           // not cut shorter than the documented cap
  CHECK(off - on <= half + 100000);  // plus the 80 ms tail
}

// ---- watchdog lock-out ------------------------------------------------------------------------------------

TEST_CASE("watchdog: a trip caused by a data flood while RTS is low does not lock RTS out") {
  Bench b;  // either
  REQUIRE(b.boot());
  b.lines(true, false);
  b.pcWrite(seqK(1000, 0));
  b.run(3000000);
  REQUIRE(b.modem->stats().watchdogTrips == 1);
  CHECK_FALSE(b.modem->lockedOut());
  b.lines(true, true);
  b.run(50000);
  CHECK(b.modem->keyed());
}

TEST_CASE("watchdog: data sent while RTS is still stuck high gets the firmware's own lead-in") {
  Bench b;  // either
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(2500000);
  REQUIRE(b.modem->stats().watchdogTrips == 1);
  REQUIRE(b.modem->lockedOut());
  b.run(1200000);  // the PC is quiet for longer than the hold-off after a trip; RTS is still stuck high
  const uint32_t t0 = b.clock.micros();
  b.pcWrite(kFrameK);
  b.run(600000);
  REQUIRE(b.uart.sent.size() == kFrameK.size());
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(t0, &on, &off));
  CHECK(b.uart.sent.front().startUs - on >= 85000);  // it is a data-keyed burst, not an RTS-keyed one
}

// ---- exact timer boundaries (the existing tests step the clock 100 us at a time and allow slack) -----------

TEST_CASE("watchdog: the carrier is cut in the very tick the limit is reached, not one later") {
  ModemConfig c = rtsModeK();
  c.maxTxUs = 300000;
  Bench b(c);
  REQUIRE(b.boot());
  b.lines(true, true);
  while (b.modem->state() == Modem::State::kRx) b.run(100);
  const uint32_t keyedAt = b.clock.micros();  // the clock value the modem stamped when the chip reported TX
  tickAt(b, keyedAt + c.maxTxUs - 1);
  CHECK(b.modem->keyed());
  CHECK(b.modem->stats().watchdogTrips == 0);
  tickAt(b, keyedAt + c.maxTxUs);
  CHECK_FALSE(b.modem->keyed());
  CHECK(b.modem->stats().watchdogTrips == 1);
}

TEST_CASE("lead-in: the first byte is released in the very tick the settle time is reached") {
  Bench b(rtsModeK());
  REQUIRE(b.boot());
  b.lines(true, true);
  while (b.modem->state() == Modem::State::kRx) b.run(100);
  const uint32_t keyedAt = b.clock.micros();
  const uint32_t lead = b.modem->config().radioSettleUs;
  tickAt(b, keyedAt + lead - 1);
  CHECK(b.modem->state() == Modem::State::kTxLead);
  tickAt(b, keyedAt + lead);
  CHECK(b.modem->state() == Modem::State::kTxData);
}

TEST_CASE("tail: data keying returns to receive in the very tick the tail time is reached") {
  ModemConfig c;
  c.keyMode = KeyMode::kData;
  Bench b(c);
  REQUIRE(b.boot());
  b.lines(true, false);
  b.pcWrite(kFrameK);
  while (b.modem->state() != Modem::State::kTxTail) b.run(100);
  const uint32_t tailStart = b.clock.micros();  // the clock value the tail was stamped with
  tickAt(b, tailStart + c.dataKeyTailUs - 1);
  CHECK(b.modem->state() == Modem::State::kTxTail);
  tickAt(b, tailStart + c.dataKeyTailUs);
  CHECK(b.modem->state() == Modem::State::kRx);
}

// ---- lead-in / settle ------------------------------------------------------------------------------------

TEST_CASE("data keying: the first byte never goes out sooner than the PLL settle time, even with a shorter lead-in configured") {
  ModemConfig c;
  c.keyMode = KeyMode::kData;
  c.dataKeyLeadUs = 0;
  Bench b(c);
  REQUIRE(b.boot());
  b.lines(true, false);
  b.pcWrite(kFrameK);
  b.run(100000);
  REQUIRE(b.uart.sent.size() == kFrameK.size());
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  CHECK(b.uart.sent.front().startUs - on >= c.radioSettleUs);
}

TEST_CASE("RTS keying: the first byte waits at least 1 ms after the chip reports TX (the default settle time)") {
  Bench b(rtsModeK());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.pcWrite(kFrameK);
  b.run(100000);
  REQUIRE(b.uart.sent.size() == kFrameK.size());
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  CHECK(b.uart.sent.front().startUs - on >= 1000);
  CHECK(b.uart.sent.front().startUs - on < 2000);
}

TEST_CASE("receive: a byte in the first 3 ms after power-up is discarded, as after any other return to receive") {
  Bench b(ModemConfig(), 100000);  // the modem is constructed well after the clock started
  REQUIRE(b.boot());
  b.lines(true, false);
  b.radioRx({0x90});
  CHECK(b.modem->stats().rxDiscardedSettle == 1);
  CHECK(b.modem->stats().rxBytes == 0);
}

// ---- receive path ----------------------------------------------------------------------------------------

TEST_CASE("receive: bytes are discarded while the chip is still being brought back to receive") {
  Bench b(rtsModeK());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(5000);
  REQUIRE(b.modem->keyed());
  b.chip.ignoreNextStrobes = 1000;  // the chip will not leave TX
  b.lines(true, false);
  b.run(10000);  // several failed recovery attempts; more than the 3 ms settle time has passed
  REQUIRE(b.chip.marc() == cc1101::kMarcTx);
  REQUIRE(b.modem->state() == Modem::State::kRx);
  b.radioRx({0x90, 0x91});
  CHECK(b.modem->stats().rxBytes == 0);
  CHECK(b.host.received.empty());
}

TEST_CASE("receive: the settle time and fault count restart after a failed return to receive is recovered") {
  Bench b(rtsModeK());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(5000);
  REQUIRE(b.modem->keyed());
  b.chip.ignoreNextStrobes = 3;  // the stop and the next two recovery attempts all fail
  b.lines(true, false);
  uint32_t guard = 0;
  while (b.chip.marc() != cc1101::kMarcRx && guard++ < 10000) b.run(100);  // retries are paced 100 ms apart
  REQUIRE(b.chip.marc() == cc1101::kMarcRx);
  CHECK(b.modem->stats().radioFaults == 3);
  b.radioRx({0x90});  // the very tick that recovered: the receiver has just come up
  CHECK(b.modem->stats().rxDiscardedSettle == 1);
  CHECK(b.host.received.empty());
}

TEST_CASE("RTS held again during recovery: the radio is not keyed in the tick that recovers it") {
  Bench b(rtsModeK());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(5000);
  REQUIRE(b.modem->keyed());
  b.chip.ignoreNextStrobes = 1;
  b.lines(true, false);
  b.clock.advance(100);
  b.modem->tick();  // RTS is down: the carrier is held for the chip's transmit latency first
  b.clock.advance(3000);
  b.modem->tick();  // the stop fails, recovery pending
  REQUIRE(b.modem->stats().radioFaults == 1);
  b.lines(true, true);
  b.clock.advance(100);
  b.modem->tick();  // recovery succeeds
  CHECK(b.chip.marc() == cc1101::kMarcRx);
  CHECK_FALSE(b.modem->keyed());
}

}  // TEST_SUITE
