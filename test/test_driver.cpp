// The CC1101 driver against the chip model: SPI framing, reset, identification, applying a profile with
// read-back, state changes, and what happens when the chip misbehaves.
#include <algorithm>

#include "bench.h"
#include "doctest.h"
#include "openhsdu/cc1101_math.h"

using namespace openhsdu;
using namespace openhsdu::cc1101;

namespace {
struct Rig {
  sim::FakeClock clock{500};
  sim::MockCc1101 chip{clock};
  Cc1101 drv{chip, clock};
};
}  // namespace

TEST_SUITE("driver") {

TEST_CASE("reset sends SRES once and the chip goes back to its reset values") {
  Rig r;
  r.chip.reg[MDMCFG4] = 0x12;
  CHECK(r.drv.reset());
  CHECK(r.chip.resets == 1);
  CHECK(r.chip.reg[MDMCFG4] == 0x8C);
  CHECK(r.chip.marc() == kMarcIdle);
}

TEST_CASE("reset gives up on a chip that never answers, without hanging") {
  Rig r;
  r.chip.stuckBusy = true;
  const uint32_t t0 = r.clock.micros();
  CHECK_FALSE(r.drv.reset());
  CHECK(elapsedUs(r.clock.micros(), t0) < 20000);
  CHECK(r.drv.timeouts() >= 1);
}

TEST_CASE("detect accepts a CC1101 of either silicon revision and nothing else") {
  Rig r;
  uint8_t part = 0xEE, ver = 0xEE;
  CHECK(r.drv.detect(&part, &ver));
  CHECK(part == 0x00);
  CHECK(ver == 0x14);
  r.chip.version = 0x04;
  CHECK(r.drv.detect());
  r.chip.version = 0x99;
  CHECK_FALSE(r.drv.detect());
  r.chip.version = 0x14;
  r.chip.partnum = 0x01;
  CHECK_FALSE(r.drv.detect());
}

TEST_CASE("detect on a chip that is not there (every read returns 0xFF) fails") {
  Rig r;
  r.chip.partnum = 0xFF;
  r.chip.version = 0xFF;
  CHECK_FALSE(r.drv.detect());
}

TEST_CASE("SPI header bytes: single write, single read, status read, PATABLE burst") {
  Rig r;
  r.drv.writeReg(MDMCFG2, 0x04);
  r.drv.readReg(MDMCFG2);
  r.drv.readStatus(MARCSTATE);
  r.drv.writePatable(0x01);
  r.drv.readPatable0();
  REQUIRE(r.chip.headers.size() >= 5);
  CHECK(r.chip.headers[0] == 0x12);          // write: bit 7 and 6 clear
  CHECK(r.chip.headers[1] == 0x92);          // read: bit 7
  CHECK(r.chip.headers[2] == 0xF5);          // status read: read + burst + 0x35
  CHECK(r.chip.headers[r.chip.headers.size() - 2] == 0x7E);  // PATABLE write burst
  CHECK(r.chip.headers.back() == 0xFE);                      // PATABLE read burst
}

TEST_CASE("PATABLE write puts the byte in entry 0 and zeros in the rest") {
  Rig r;
  r.drv.writePatable(0x02);
  CHECK(r.chip.patable[0] == 0x02);
  for (int i = 1; i < 8; ++i) CHECK(r.chip.patable[i] == 0x00);
  CHECK(r.drv.readPatable0() == 0x02);
}

TEST_CASE("apply writes every register of the profile and reads each one back") {
  Rig r;
  r.drv.reset();
  const ApplyResult res = r.drv.apply(profileProven(), 0x01);
  CHECK(res.ok);
  CHECK(res.mismatches == 0);
  CHECK(res.patableOk);
  for (size_t i = 0; i < profileProven().count; ++i) CHECK(r.chip.reg[profileProven().regs[i].addr] == profileProven().regs[i].value);
  CHECK(r.chip.patable[0] == 0x01);
}

TEST_CASE("apply leaves the registers outside the profile at their reset values") {
  Rig r;
  r.drv.reset();
  r.drv.apply(profileProven(), 0x01);
  CHECK(r.chip.reg[SYNC1] == 0xD3);
  CHECK(r.chip.reg[PKTLEN] == 0xFF);
  CHECK(r.chip.reg[ADDR] == 0x00);
}

TEST_CASE("apply reports a register that will not take its value") {
  Rig r;
  r.drv.reset();
  r.chip.stuckReg[MDMCFG3] = true;  // reset value 0x22, the profile wants 0x83
  const ApplyResult res = r.drv.apply(profileProven(), 0x01);
  CHECK_FALSE(res.ok);
  CHECK(res.mismatches == 1);
  CHECK(res.firstBadAddr == MDMCFG3);
}

TEST_CASE("apply counts every bad register and keeps the first address") {
  Rig r;
  r.drv.reset();
  r.chip.stuckReg[FREQ1] = true;
  r.chip.stuckReg[MDMCFG3] = true;
  const ApplyResult res = r.drv.apply(profileProven(), 0x01);
  CHECK(res.mismatches == 2);
  CHECK(res.firstBadAddr == FREQ1);  // lower address in the profile's order comes first
}

TEST_CASE("apply notices a PATABLE that does not read back") {
  Rig r;
  r.drv.reset();
  r.chip.readBackWrongPatable = true;
  const ApplyResult res = r.drv.apply(profileProven(), 0x01);
  CHECK_FALSE(res.patableOk);
  CHECK_FALSE(res.ok);
  CHECK(res.mismatches == 0);
}

TEST_CASE("enterTx goes through idle first, then transmit, and takes one calibration") {
  Rig r;
  r.drv.reset();
  r.drv.apply(profileProven(), 0x01);
  r.chip.strobes.clear();
  const uint32_t t0 = r.clock.micros();
  CHECK(r.drv.enterTx());
  REQUIRE(r.chip.strobes.size() == 2);
  CHECK(r.chip.strobes[0].cmd == SIDLE);
  CHECK(r.chip.strobes[1].cmd == STX);
  CHECK(r.chip.marc() == kMarcTx);
  const uint32_t took = elapsedUs(r.clock.micros(), t0);
  CHECK(took >= 700);
  CHECK(took < 1500);  // the datasheet's ~720 us calibration plus a few polls
}

TEST_CASE("enterRx likewise") {
  Rig r;
  r.drv.reset();
  r.drv.apply(profileProven(), 0x01);
  r.chip.strobes.clear();
  CHECK(r.drv.enterRx());
  REQUIRE(r.chip.strobes.size() == 2);
  CHECK(r.chip.strobes[0].cmd == SIDLE);
  CHECK(r.chip.strobes[1].cmd == SRX);
  CHECK(r.chip.marc() == kMarcRx);
}

TEST_CASE("without autocalibration the switch is faster") {
  Rig r;
  r.drv.reset();
  const uint32_t t0 = r.clock.micros();
  CHECK(r.drv.enterTx());  // reset MCSM0 = 0x04: no calibration
  CHECK(elapsedUs(r.clock.micros(), t0) < 400);
}

TEST_CASE("a transmit request while receiving first drops to idle, so a stale clear-channel check cannot stop it") {
  Rig r;
  r.drv.reset();
  r.drv.apply(profileProven(), 0x01);
  r.drv.enterRx();
  r.chip.strobes.clear();
  CHECK(r.drv.enterTx());
  CHECK(r.chip.strobes.front().cmd == SIDLE);
}

TEST_CASE("enterTx and enterRx fail, with a timeout, when the chip ignores the strobe") {
  Rig r;
  r.drv.reset();
  r.chip.freezeState = true;
  const uint32_t t0 = r.clock.micros();
  CHECK_FALSE(r.drv.enterTx());
  CHECK(elapsedUs(r.clock.micros(), t0) < 20000);
  CHECK(r.drv.timeouts() >= 1);
  const uint32_t before = r.drv.timeouts();
  CHECK_FALSE(r.drv.enterRx());
  CHECK(r.drv.timeouts() > before);
}

TEST_CASE("a lost SIDLE is harmless when the chip is already idle") {
  Rig r;
  r.drv.reset();
  r.drv.apply(profileProven(), 0x01);
  r.chip.ignoreNextStrobes = 1;
  CHECK(r.drv.enterTx());
}

TEST_CASE("a lost SIDLE is fatal when the chip is transmitting: the driver reports failure, not success") {
  Rig r;
  r.drv.reset();
  r.drv.apply(profileProven(), 0x01);
  REQUIRE(r.drv.enterTx());
  r.chip.ignoreNextStrobes = 1;
  CHECK_FALSE(r.drv.enterRx());      // the chip is still in TX
  CHECK(r.chip.marc() == kMarcTx);
  CHECK(r.drv.enterRx());            // and the next attempt works
}

TEST_CASE("a corrupt status read is filtered out by reading until two reads agree") {
  Rig r;
  r.chip.glitchNextMarcRead = true;
  CHECK(r.drv.marcState() == kMarcIdle);
}

TEST_CASE("every transaction is framed by chip select") {
  Rig r;
  const int before = r.chip.selectCount;
  r.drv.writeReg(PKTCTRL0, 0x32);
  r.drv.readReg(PKTCTRL0);
  CHECK(r.chip.selectCount - before == 2);
}

TEST_CASE("strobe returns 0xFF if the chip never becomes ready") {
  Rig r;
  r.chip.stuckBusy = true;
  CHECK(r.drv.strobe(SIDLE) == 0xFF);
}

TEST_CASE("readReg on a dead chip returns 0xFF and does not hang") {
  Rig r;
  r.chip.stuckBusy = true;
  CHECK(r.drv.readReg(PKTCTRL0) == 0xFF);
}

}  // TEST_SUITE
