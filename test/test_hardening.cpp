// Regression tests for the problems an independent review found (datasheet, firmware, logic and docs reviewers),
// one test per confirmed finding. Each of these failed against the first version of the code.
#include <ios>
#include <algorithm>

#include "bench.h"
#include "doctest.h"
#include "openhsdu/cc1101_math.h"
#include "openhsdu/console.h"
#include "openhsdu/power.h"

using namespace openhsdu;
using namespace openhsdu::cc1101;
using sim::Bench;

namespace {

const std::vector<uint8_t> kPayload = {0x31, 0x32, 0x33, 0x34, 0x35};

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

// An idle loop that keeps ticking once a second, as the firmware does, for `seconds` of fake time.
void idleSeconds(Bench& b, uint32_t seconds) {
  for (uint32_t i = 0; i < seconds; ++i) {
    b.clock.advance(1000000);
    b.modem->tick();
  }
}

// The datasheet's reset value of every configuration register (SWRS061, register overview).
const uint8_t kDatasheetReset[kNumConfigRegs] = {
    0x29, 0x2E, 0x3F, 0x07, 0xD3, 0x91, 0xFF, 0x04, 0x45, 0x00, 0x00, 0x0F, 0x00, 0x1E, 0xC4, 0xEC,
    0x8C, 0x22, 0x02, 0x22, 0xF8, 0x47, 0x07, 0x30, 0x04, 0x36, 0x6C, 0x03, 0x40, 0x91, 0x87, 0x6B,
    0xF8, 0x56, 0x10, 0xA9, 0x0A, 0x20, 0x0D, 0x41, 0x00, 0x59, 0x7F, 0x3F, 0x88, 0x31, 0x0B};

}  // namespace

TEST_SUITE("hardening") {

// ---- a radio that was not verified must never transmit ---------------------------------------------------

TEST_CASE("a modem that was never started cannot key, however it is asked") {
  Bench b;  // no boot()
  b.lines(true, true);
  b.pcWrite(kPayload);
  b.run(500000);
  CHECK_FALSE(b.modem->radioReady());
  CHECK(b.txWindows() == 0);
  CHECK(b.uart.sent.empty());
  CHECK(b.modem->stats().txDroppedFault == kPayload.size());
  const uint8_t x = 1;
  CHECK_FALSE(b.modem->carrierBurst(1000));
  CHECK_FALSE(b.modem->sendBytes(&x, 1));
}

TEST_CASE("a failed boot (not a CC1101) leaves the modem unable to key") {
  Bench b;
  b.chip.version = 0x77;
  REQUIRE_FALSE(b.boot());
  b.lines(true, true);
  b.pcWrite(kPayload);
  b.run(500000);
  CHECK(b.txWindows() == 0);
  CHECK_FALSE(b.modem->radioReady());
}

TEST_CASE("a failed boot (registers that will not take their values) leaves the modem unable to key") {
  Bench b;
  b.chip.stuckReg[FREQ0] = true;
  REQUIRE_FALSE(b.boot());
  b.lines(true, true);
  b.run(500000);
  CHECK(b.txWindows() == 0);
}

TEST_CASE("a boot above the power cap is refused and cannot key") {
  Bench b;
  REQUIRE_FALSE(b.boot(profileProven(), 0xC0, kDefaultMaxChipDbm));
  b.lines(true, true);
  b.run(500000);
  CHECK(b.txWindows() == 0);
}

TEST_CASE("a second boot that fails switches the modem off again") {
  Bench b;
  REQUIRE(b.boot());
  CHECK(b.modem->radioReady());
  REQUIRE_FALSE(b.boot(profileProven(), 0xC0, kDefaultMaxChipDbm));
  CHECK_FALSE(b.modem->radioReady());
  CHECK_FALSE(b.device->present());
}

TEST_CASE("a profile change that fails is undone, and the radio keeps working") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.chip.stuckReg[FREQ0] = true;  // the new profile's FREQ0 (0xCF) will not stick; the old one (0xB1) is still there
  CHECK_FALSE(b.device->setProfile(profileHsduMatched()));
  CHECK(b.device->present());
  CHECK(std::string(b.device->profile().name) == "proven");
  CHECK(b.modem->radioReady());
}

TEST_CASE("a profile change that fails and cannot be undone disables the radio until a later change succeeds") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.chip.reg[FREQ0] = 0x00;       // drifted
  b.chip.stuckReg[FREQ0] = true;  // and will not be written
  CHECK_FALSE(b.device->setProfile(profileHsduMatched()));
  CHECK_FALSE(b.device->present());
  CHECK_FALSE(b.modem->radioReady());
  b.lines(true, true);
  b.run(300000);
  CHECK(b.txWindows() == 0);  // nothing transmits in a half-configured state
  b.chip.stuckReg[FREQ0] = false;
  CHECK(b.device->setProfile(profileProven()));  // a later change doubles as the retry
  CHECK(b.modem->radioReady());
  b.lines(true, false);
  b.lines(true, true);
  b.run(50000);
  CHECK(b.modem->keyed());
}

TEST_CASE("a power change that does not read back disables the radio") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.chip.readBackWrongPatable = true;
  CHECK_FALSE(b.device->setPatable(0x02));
  CHECK_FALSE(b.device->present());
  CHECK_FALSE(b.modem->radioReady());
  b.chip.readBackWrongPatable = false;
  CHECK(b.device->setProfile(profileProven()));
  CHECK(b.modem->radioReady());
}

TEST_CASE("setProfile before a successful boot is refused") {
  Bench b;
  CHECK_FALSE(b.device->setProfile(profileHsduMatched()));
  CHECK(b.chip.reg[FREQ0] != 0xCF);
}

TEST_CASE("setProfile will not touch a chip that is not a CC1101") {
  Bench b;
  REQUIRE(b.boot());
  b.chip.version = 0x55;
  CHECK_FALSE(b.device->setProfile(profileHsduMatched()));
  CHECK(b.chip.reg[FREQ0] == 0xB1);
  CHECK_FALSE(b.modem->radioReady());
  CHECK_FALSE(b.device->present());
}

// ---- the self-test must tolerate what calibration writes -------------------------------------------------

TEST_CASE("selftest passes after calibration has rewritten FSCAL3, FSCAL2 and FSCAL1") {
  Bench b;
  REQUIRE(b.boot());
  REQUIRE(b.chip.calibrations >= 1);
  CHECK(b.chip.reg[FSCAL1] == 0x15);                // the chip's result, not the profile's 0x00
  CHECK((b.chip.reg[FSCAL3] & 0x0F) == 0x07);       // result bits differ from the profile's 0xE9
  SelfTestReport r;
  REQUIRE(b.device->selfTest(r));
  CHECK(r.mismatches == 0);
  CHECK(r.pass);
}

TEST_CASE("selftest still catches a drifted INPUT bit of the calibration registers") {
  Bench b;
  REQUIRE(b.boot());
  SelfTestReport r;
  b.chip.reg[FSCAL3] = 0x07;  // high nibble is an input: the profile says 0xE
  REQUIRE(b.device->selfTest(r));
  CHECK_FALSE(r.pass);
  CHECK(r.firstBadAddr == FSCAL3);
}

TEST_CASE("selftest reports a synthesizer that did not lock (FSCAL1 = 0x3F)") {
  Bench b;
  REQUIRE(b.boot());
  b.chip.reg[FSCAL1] = 0x3F;
  SelfTestReport r;
  REQUIRE(b.device->selfTest(r));
  CHECK_FALSE(r.pass);
  CHECK(r.firstBadAddr == FSCAL1);
}

TEST_CASE("selftest still compares every other register exactly, including FSCAL0 and TEST2..0") {
  for (uint8_t addr : {static_cast<uint8_t>(FSCAL0), static_cast<uint8_t>(TEST2), static_cast<uint8_t>(TEST1),
                       static_cast<uint8_t>(TEST0), static_cast<uint8_t>(MDMCFG4)}) {
    Bench b;
    REQUIRE(b.boot());
    b.chip.reg[addr] ^= 0x01;
    SelfTestReport r;
    REQUIRE(b.device->selfTest(r));
    INFO("addr 0x" << std::hex << int(addr));
    CHECK_FALSE(r.pass);
    CHECK(r.firstBadAddr == addr);
  }
}

// ---- the chip's transmit latency -------------------------------------------------------------------------

TEST_CASE("RTS released the instant the last byte leaves the UART: the carrier outlives the chip's 8-bit latency") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(5000);
  b.pcWrite(std::vector<uint8_t>(20, 0x55));
  while (b.uart.busy() || b.uart.sent.empty()) b.run(50);
  b.lines(true, false);
  b.run(20000);
  CHECK(b.allBytesSentInTx(1667));  // 8 bit periods at 4800 baud
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  REQUIRE(off != 0);
  CHECK(off - b.uart.sent.back().endUs >= 3000);
}

TEST_CASE("the same test fails with the hold switched off, so it can tell") {
  ModemConfig c = rtsMode();
  c.txLatencyHoldUs = 0;
  Bench b(c);
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(5000);
  b.pcWrite(std::vector<uint8_t>(20, 0x55));
  while (b.uart.busy() || b.uart.sent.empty()) b.run(50);
  b.lines(true, false);
  b.run(20000);
  CHECK_FALSE(b.allBytesSentInTx(1667));
}

TEST_CASE("data keying also leaves the carrier up past the chip's latency") {
  Bench b(dataMode());
  REQUIRE(b.boot());
  b.lines(true, false);
  b.pcWrite(std::vector<uint8_t>(20, 0x55));
  b.run(600000);
  CHECK(b.allBytesSentInTx(1667));
}

// ---- timers that outlive their purpose (32-bit microsecond clock, 35.8 min) -----------------------------

TEST_CASE("a receiver that has been idle for 40, 72 and 100 minutes still delivers what it hears") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  b.run(10000);
  const uint32_t checkpoints[] = {40 * 60, 72 * 60 - 40 * 60, 100 * 60 - 72 * 60};  // seconds to idle before each check
  uint8_t value = 0x80;
  for (uint32_t secs : checkpoints) {
    idleSeconds(b, secs);
    b.host.received.clear();
    b.radioRx({value});
    b.run(1000);
    INFO("after another " << secs << " s");
    CHECK(b.host.received == std::vector<uint8_t>{value});
    ++value;
  }
  CHECK(b.modem->stats().rxDiscardedSettle == 0);
}

TEST_CASE("after a failed key-up and 40 idle minutes the radio can transmit again at once") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.chip.loseTx = true;
  b.run(5000);
  REQUIRE(b.modem->stats().keyUpFailures >= 1);
  b.chip.loseTx = false;
  b.lines(true, false);
  b.run(300000);
  idleSeconds(b, 40 * 60);
  b.lines(true, true);
  b.run(20000);
  CHECK(b.modem->keyed());
}

TEST_CASE("a bench carrier that expired without ever keying does not key the radio 40 minutes later") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  REQUIRE(b.modem->carrierBurst(100000));
  b.modem->setRadioReady(false);  // it is accepted, then the radio is declared unusable before it can key
  b.run(1000000);                 // the 100 ms request expires
  b.modem->setRadioReady(true);
  idleSeconds(b, 40 * 60);
  b.run(1000000);
  CHECK(b.txWindows() == 0);
  CHECK_FALSE(b.modem->keyed());
}

// ---- the watchdog cannot be defeated by a stream ---------------------------------------------------------

TEST_CASE("a host that streams bytes forever gets one 2 s carrier, not one every 2 s") {
  Bench b;  // either
  REQUIRE(b.boot());
  b.lines(true, false);
  for (int i = 0; i < 3000; ++i) {  // 30 seconds of a 3-byte write every 10 ms
    b.pcWrite({0x41, 0x42, 0x43});
    b.run(10000);
  }
  CHECK(b.modem->stats().watchdogTrips == 1);
  CHECK(b.txWindows() == 1);
  CHECK(b.modem->stats().txDroppedFault > 0);
  CHECK(b.modem->lockedOut());
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  REQUIRE(off != 0);
  CHECK(off - on <= 2010000);
  // the stream stops; after a second of quiet the radio can be keyed again
  b.run(1200000);
  CHECK_FALSE(b.modem->lockedOut());
  b.pcWrite(kPayload);
  b.run(600000);
  CHECK(b.txWindows() == 2);
}

TEST_CASE("RTS stuck high plus streaming data cannot re-key either") {
  Bench b;  // either
  REQUIRE(b.boot());
  b.lines(true, true);
  for (int i = 0; i < 1000; ++i) {
    b.pcWrite({0x41});
    b.run(10000);
  }
  CHECK(b.modem->stats().watchdogTrips == 1);
  CHECK(b.txWindows() == 1);
}

// ---- a chip that is transmitting when it should not be ---------------------------------------------------

TEST_CASE("a chip found transmitting while the modem thinks it is receiving is told to listen, within about 20 ms") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  b.run(1000);
  REQUIRE(b.driver.enterTx());  // something other than the modem keyed it
  REQUIRE(b.chip.marc() == kMarcTx);
  b.run(60000);
  CHECK(b.modem->stats().unexpectedTx == 1);
  CHECK(b.chip.marc() == kMarcRx);
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  REQUIRE(off != 0);
  CHECK(off - on <= 40000);
}

TEST_CASE("a key-up that times out half way and whose recovery strobe is lost still ends with the chip listening") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.chip.calUs = 10000;          // slower than the driver's 6 ms wait: the key-up times out with TX still pending
  b.chip.dropStrobes(2, 1);      // SIDLE and STX go through; the SIDLE that follows is lost, so TX completes anyway
  b.lines(true, true);
  b.clock.advance(100);
  b.modem->tick();               // the one tick that tries to key up (and takes ~8 ms of chip time)
  REQUIRE(b.modem->stats().keyUpFailures == 1);
  b.clock.advance(12000);  // the chip finishes calibrating on its own and reaches TX behind the modem's back
  REQUIRE(b.chip.marc() == kMarcTx);
  b.lines(true, false);
  b.chip.calUs = 799;
  b.run(300000);
  CHECK(b.chip.marc() == kMarcRx);
  CHECK_FALSE(b.modem->keyed());
  CHECK(b.modem->stats().radioFaults >= 2);
  uint32_t on = 0, off = 0;
  REQUIRE(b.firstTxWindow(0, &on, &off));
  REQUIRE(off != 0);
  CHECK(off - on < 80000);  // the stray carrier was found and ended within a few retries
}

// ---- requests that were withdrawn ------------------------------------------------------------------------

// ---- second review: seams between the pieces ------------------------------------------------------------

TEST_CASE("a host that frames each transmission with RTS gets the radio back after a trip, even while it keeps polling") {
  for (KeyMode mode : {KeyMode::kRts, KeyMode::kEither}) {
    ModemConfig c;
    c.keyMode = mode;
    Bench b(c);
    REQUIRE(b.boot());
    b.lines(true, true);
    b.run(2500000);  // RTS stuck for more than the 2 s limit: one trip
    REQUIRE(b.modem->stats().watchdogTrips == 1);
    b.lines(true, false);
    b.run(10000);
    for (int i = 0; i < 10; ++i) {  // a poll every ~300 ms, each framed by RTS as a real host does
      b.lines(true, true);
      b.run(80000);
      b.pcWrite({0x41, 0x42});
      b.run(40000);
      b.lines(true, false);
      b.run(180000);
    }
    INFO("mode " << int(mode));
    CHECK(b.uart.sent.size() == 20);  // every poll was sent
    CHECK(b.txWindows() == 11);       // the stuck carrier and the ten polls
    CHECK(b.modem->stats().watchdogTrips == 1);
  }
}

TEST_CASE("key mode data: pulsing RTS does not clear the hold-off after a trip, so a stream still gets one carrier") {
  Bench b(dataMode());
  REQUIRE(b.boot());
  b.lines(true, false);
  for (int i = 0; i < 3000; ++i) {
    if (i % 10 == 0) b.lines(true, (i / 10) % 2 == 0);
    b.pcWrite({0x41, 0x42, 0x43});
    b.run(10000);
  }
  CHECK(b.modem->stats().watchdogTrips == 1);
  CHECK(b.txWindows() == 1);
}

TEST_CASE("setProfile cannot make a radio ready that never completed begin()") {
  Bench b;
  b.chip.version = 0x77;
  REQUIRE_FALSE(b.boot());
  b.chip.version = kVersionCurrent;  // the wiring is fixed, but the firmware has not connected its transmit pin
  CHECK_FALSE(b.device->setProfile(profileProven()));
  CHECK_FALSE(b.modem->radioReady());
  CHECK_FALSE(b.device->present());
}

TEST_CASE("a failed second begin() also blocks setProfile") {
  Bench b;
  REQUIRE(b.boot());
  REQUIRE_FALSE(b.boot(profileProven(), 0xC0, kDefaultMaxChipDbm));
  CHECK_FALSE(b.device->setProfile(profileProven()));
  CHECK_FALSE(b.modem->radioReady());
}

TEST_CASE("begin() is refused, and the chip left alone, while the transmitter is keyed") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(10000);
  REQUIRE(b.modem->keyed());
  const int resets = b.chip.resets;
  CHECK_FALSE(b.device->begin(profileProven(), kDefaultPatable, kDefaultMaxChipDbm));
  CHECK(b.chip.resets == resets);
  CHECK(b.modem->keyed());
}

TEST_CASE("bytes written while the chip cannot be confirmed in receive are dropped, not sent whenever it recovers") {
  Bench b;  // either
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(10000);
  REQUIRE(b.modem->keyed());
  b.chip.ignoreNextStrobes = 1000;  // the chip will not leave TX
  b.lines(true, false);
  b.run(10000);
  REQUIRE(b.chip.marc() == kMarcTx);
  REQUIRE_FALSE(b.modem->keyed());   // the modem believes it is receiving, and is retrying
  b.pcWrite(kPayload);
  CHECK(b.modem->stats().txDroppedFault == kPayload.size());
  b.chip.ignoreNextStrobes = 0;
  b.run(300000);
  CHECK(b.chip.marc() == kMarcRx);
  CHECK(b.uart.sent.empty());        // nothing leaks out after the recovery
}

TEST_CASE("the 20 ms stuck-carrier check survives the loop stalling for 40 minutes") {
  Bench b;
  REQUIRE(b.boot());
  b.lines(true, false);
  b.run(1000);
  b.clock.advance(2400000000u);  // 40 minutes with no tick at all
  b.modem->tick();
  REQUIRE(b.driver.enterTx());   // something keys the chip behind the modem's back
  b.run(100000);
  CHECK(b.modem->stats().unexpectedTx == 1);
  CHECK(b.chip.marc() == kMarcRx);
}

TEST_CASE("RTS keying: a request withdrawn before the radio keyed is dropped and does not leak into the next one") {
  Bench b(rtsMode());
  REQUIRE(b.boot());
  b.lines(true, true);
  b.pcWrite(kPayload);
  b.lines(true, false);  // RTS falls before any tick: the radio was never keyed
  b.run(500000);
  CHECK(b.uart.sent.empty());
  CHECK(b.txWindows() == 0);
  CHECK(b.modem->stats().keyUps == 0);
  CHECK(b.modem->stats().txDroppedNoKey == kPayload.size());
  // the next, proper request carries only its own bytes
  b.lines(true, true);
  b.run(5000);
  b.pcWrite({0x61, 0x62});
  b.lines(true, false);
  b.run(300000);
  CHECK(b.uart.bytes() == std::vector<uint8_t>{0x61, 0x62});
}

// ---- the chip model and the driver ----------------------------------------------------------------------

TEST_CASE("after a reset every configuration register has the datasheet's reset value") {
  sim::FakeClock clock(500);
  sim::MockCc1101 chip(clock);
  Cc1101 drv(chip, clock);
  for (int i = 0; i < kNumConfigRegs; ++i) chip.reg[i] = static_cast<uint8_t>(0xA5 + i);
  REQUIRE(drv.reset());
  for (uint8_t i = 0; i < kNumConfigRegs; ++i) {
    INFO("register 0x" << std::hex << int(i));
    CHECK(chip.reg[i] == kDatasheetReset[i]);
  }
}

TEST_CASE("the profile's WORCTRL is a real change from the reset value, and FREND1 is not") {
  // WORCTRL: reset 0xF8, profile 0xFB. FREND1: reset 0x56, profile 0x56 (written anyway: harmless, and explicit).
  CHECK(kDatasheetReset[WORCTRL] == 0xF8);
  CHECK(kDatasheetReset[FREND1] == 0x56);
  bool worctrl = false, frend1 = false;
  for (size_t i = 0; i < profileProven().count; ++i) {
    if (profileProven().regs[i].addr == WORCTRL) worctrl = profileProven().regs[i].value == 0xFB;
    if (profileProven().regs[i].addr == FREND1) frend1 = profileProven().regs[i].value == 0x56;
  }
  CHECK(worctrl);
  CHECK(frend1);
}

TEST_CASE("reading or writing 0x30..0x3D as a 'register' never sends a command strobe") {
  sim::FakeClock clock(500);
  sim::MockCc1101 chip(clock);
  Cc1101 drv(chip, clock);
  CHECK(drv.readReg(0x30) == 0xFF);  // would be SRES
  CHECK(drv.readReg(0x34) == 0xFF);  // would be SRX
  drv.writeReg(0x35, 0x00);          // would be STX
  drv.writeReg(0x36, 0x00);          // would be SIDLE
  CHECK(drv.readReg(kNumConfigRegs) == 0xFF);
  CHECK(chip.strobes.empty());
  CHECK(chip.resets == 0);
}

TEST_CASE("every driver operation ends with chip select released, and none starts while it is still low") {
  sim::FakeClock clock(500);
  sim::MockCc1101 chip(clock);
  Cc1101 drv(chip, clock);
  drv.reset();
  CHECK_FALSE(chip.selected());
  drv.detect();
  CHECK_FALSE(chip.selected());
  drv.writeReg(PKTCTRL0, 0x32);
  drv.readReg(PKTCTRL0);
  CHECK_FALSE(chip.selected());
  drv.readStatus(MARCSTATE);
  CHECK_FALSE(chip.selected());
  drv.writePatable(0x01);
  CHECK_FALSE(chip.selected());
  drv.readPatable0();
  CHECK_FALSE(chip.selected());
  drv.strobe(SIDLE);
  CHECK_FALSE(chip.selected());
  drv.apply(profileProven(), 0x01);
  drv.enterTx();
  drv.enterRx();
  drv.rssiDbm();
  CHECK_FALSE(chip.selected());
  CHECK(chip.csViolations == 0);
}

TEST_CASE("a chip that does not answer is also left deselected") {
  sim::FakeClock clock(500);
  sim::MockCc1101 chip(clock);
  Cc1101 drv(chip, clock);
  chip.stuckBusy = true;
  drv.reset();
  drv.readReg(PKTCTRL0);
  drv.writeReg(PKTCTRL0, 1);
  drv.strobe(SIDLE);
  drv.writePatable(1);
  drv.readPatable0();
  drv.readStatus(MARCSTATE);
  CHECK_FALSE(chip.selected());
  CHECK(chip.csViolations == 0);
}

TEST_CASE("reset leaves a margin of at least 100 us after the reset strobe before the chip is used") {
  sim::FakeClock clock(500);
  sim::MockCc1101 chip(clock);
  Cc1101 drv(chip, clock);
  REQUIRE(drv.reset());
  REQUIRE_FALSE(chip.strobes.empty());
  CHECK(chip.strobes.back().cmd == SRES);
  CHECK(elapsedUs(clock.micros(), chip.strobes.back().t) >= 100);
}

TEST_CASE("the chip's own transmit state is readable") {
  sim::FakeClock clock(500);
  sim::MockCc1101 chip(clock);
  Cc1101 drv(chip, clock);
  drv.reset();
  CHECK_FALSE(drv.transmitting());
  REQUIRE(drv.enterTx());
  CHECK(drv.transmitting());
  REQUIRE(drv.enterRx());
  CHECK_FALSE(drv.transmitting());
}

TEST_CASE("RSSI conversion: datasheet formula with the 74 dB offset") {
  CHECK(Cc1101::rssiDbmFromRaw(0x00) == doctest::Approx(-74.0));
  CHECK(Cc1101::rssiDbmFromRaw(0x28) == doctest::Approx(-54.0));   // +40 raw = +20 dB
  CHECK(Cc1101::rssiDbmFromRaw(0xC0) == doctest::Approx(-106.0));  // -64 raw = -32 dB below the offset
  CHECK(Cc1101::rssiDbmFromRaw(0x80) == doctest::Approx(-138.0));
}

TEST_CASE("the profiles satisfy every register condition asynchronous serial mode needs") {
  for (const Profile* p : {&profileProven(), &profileHsduMatched()}) {
    INFO(p->name);
    auto val = [&](uint8_t addr) {
      for (size_t i = 0; i < p->count; ++i)
        if (p->regs[i].addr == addr) return p->regs[i].value;
      return static_cast<uint8_t>(0xFF);
    };
    CHECK((val(PKTCTRL0) & 0x40) == 0);               // WHITE_DATA off (its reset value is on)
    CHECK((val(PKTCTRL0) & 0x04) == 0);               // CRC off
    CHECK((val(PKTCTRL0) & 0x03) == 0x02);            // infinite packet length
    CHECK((val(MDMCFG2) & 0x08) == 0);                // Manchester off
    CHECK((val(MDMCFG1) & 0x80) == 0);                // forward error correction off
    CHECK((val(MCSM1) & 0x0F) == 0);                  // RXOFF and TXOFF modes: idle
    CHECK((val(MDMCFG2) & 0x07) == 0x04);             // sync mode as in the proven preset
  }
}

TEST_CASE("the power table values are pinned, so a quiet edit of a step is noticed") {
  const struct { uint8_t patable; double dbm; } expect[] = {
      {0x01, -36.5}, {0x02, -33.8}, {0x11, -33.8}, {0x12, -30.0}, {0x0E, -20.0}, {0x1D, -15.0},
      {0x34, -10.0}, {0x60, 0.0},   {0x84, 5.0},   {0xC8, 7.0},   {0xC0, 10.0}};
  for (const auto& e : expect) {
    REQUIRE(findPowerStep(e.patable) != nullptr);
    CHECK(findPowerStep(e.patable)->chipDbm == doctest::Approx(e.dbm));
  }
}

// ---- console additions ------------------------------------------------------------------------------------

namespace {
struct ConsoleRig {
  Bench b;
  sim::FakeConsoleOut out;
  Console con;
  ConsoleRig() : b(), con(*b.device, *b.modem, out) {
    REQUIRE(b.boot());
    b.lines(true, false);
    b.run(10000);
  }
  std::string say(const char* line) {
    out.clear();
    con.feedString(line);
    con.feedString("\n");
    return out.text();
  }
};
}  // namespace

TEST_CASE("console rssi prints the raw value and an approximate level") {
  ConsoleRig r;
  const std::string s = r.say("rssi");
  CHECK(s.find("rssi raw=0x") != std::string::npos);
  CHECK(s.find("dBm") != std::string::npos);
}

TEST_CASE("console rssi is refused while transmitting and with no radio") {
  ConsoleRig r;
  r.b.lines(true, true);
  r.b.run(20000);
  REQUIRE(r.b.modem->keyed());
  CHECK(r.say("rssi").find("error: transmitting") != std::string::npos);
}

TEST_CASE("console rssi, carrier and pattern say so when the radio is missing") {
  Bench b;
  b.chip.version = 0x77;
  REQUIRE_FALSE(b.boot());
  sim::FakeConsoleOut out;
  Console con(*b.device, *b.modem, out);
  con.feedString("rssi\n");
  CHECK(out.has("error: radio missing"));
  out.clear();
  con.feedString("carrier 100\n");
  CHECK(out.has("error: busy or radio missing"));
  out.clear();
  con.feedString("pattern 4\n");
  CHECK(out.has("error: busy or radio missing"));
}

TEST_CASE("console refuses a line that contains a NUL instead of silently cutting it short") {
  ConsoleRig r;
  r.out.clear();
  const uint8_t line[] = {'r', 'e', 'g', ' ', '0', '8', 0x00, 'Z', 'Z', '\n'};
  r.con.feed(line, sizeof(line));
  CHECK(r.out.has("not text"));
  CHECK_FALSE(r.out.has("reg 0x08"));
}

TEST_CASE("console status includes the unexpected-transmit counter") {
  ConsoleRig r;
  CHECK(r.say("status").find("unexpected-tx=0") != std::string::npos);
}

TEST_CASE("console carrier lasts the requested time plus the lead-in and tail") {
  ConsoleRig r;
  CHECK(r.say("carrier 20").find("ok carrier 20 ms") != std::string::npos);
  r.b.run(1000000);
  uint32_t on = 0, off = 0;
  REQUIRE(r.b.firstTxWindow(0, &on, &off));
  REQUIRE(off != 0);
  CHECK(off - on >= 160000);  // 85 ms lead + 80 ms tail dominate a 20 ms request
}

}  // TEST_SUITE
