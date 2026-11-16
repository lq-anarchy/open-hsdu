// Tests that kill the driver/device mutants the existing suite lets through. Each one passes on the unmodified code.
#include <utility>
#include <vector>

#include "bench.h"
#include "doctest.h"
#include "openhsdu/cc1101_math.h"
#include "openhsdu/ring_buffer.h"

using namespace openhsdu;
using namespace openhsdu::cc1101;
using sim::Bench;

namespace {

struct KRig {
  sim::FakeClock clock{500};
  sim::MockCc1101 chip{clock};
  Cc1101 drv{chip, clock};
};

// A chip model whose MARCSTATE reads can be corrupted several times in a row.
struct GlitchyChip : sim::MockCc1101 {
  explicit GlitchyChip(sim::FakeClock& c) : sim::MockCc1101(c) {}
  int garbageReads = 0;
  bool nextIsMarcData = false;
  uint8_t transfer(uint8_t out) override {
    const uint8_t v = sim::MockCc1101::transfer(out);
    if (nextIsMarcData) {
      nextIsMarcData = false;
      if (garbageReads > 0) return static_cast<uint8_t>(0x10 + garbageReads--);  // distinct each time
      return v;
    }
    if (out == (kHeaderRead | kHeaderBurst | MARCSTATE)) nextIsMarcData = true;
    return v;
  }
};

// A chip model that remembers when chip select moved.
struct TimedChip : sim::MockCc1101 {
  explicit TimedChip(sim::FakeClock& c) : sim::MockCc1101(c), clk(c) {}
  sim::FakeClock& clk;
  std::vector<std::pair<bool, uint32_t>> selects;
  void select(bool active) override {
    selects.emplace_back(active, clk.micros());
    sim::MockCc1101::select(active);
  }
};

}  // namespace

TEST_SUITE("mutation-kills") {

TEST_CASE("driver: a chip that never answers is given about 3 ms per wait, not less and not unbounded") {
  KRig r;
  r.chip.stuckBusy = true;
  const uint32_t t0 = r.clock.micros();
  CHECK(r.drv.strobe(SIDLE) == 0xFF);
  const uint32_t took = elapsedUs(r.clock.micros(), t0);
  CHECK(took > 3000);   // waits out the whole 3 ms
  CHECK(took <= 3100);  // and gives up promptly after it
}

TEST_CASE("driver: a lost SIDLE is waited for about 2 ms before the driver gives up") {
  KRig r;
  r.drv.reset();
  r.drv.apply(profileProven(), 0x01);
  REQUIRE(r.drv.enterTx());
  r.chip.ignoreNextStrobes = 1;
  const uint32_t t0 = r.clock.micros();
  CHECK_FALSE(r.drv.enterIdle());
  const uint32_t took = elapsedUs(r.clock.micros(), t0);
  CHECK(took >= 2000);
  CHECK(took <= 2200);
}

TEST_CASE("driver: a slow calibration (5 ms) is still waited for") {
  KRig r;
  r.drv.reset();
  r.drv.apply(profileProven(), 0x01);
  r.chip.calUs = 5000;
  CHECK(r.drv.enterTx());
  CHECK(r.chip.marc() == kMarcTx);
}

TEST_CASE("driver: a MARCSTATE read that is wrong several times in a row is still filtered out") {
  sim::FakeClock clock(500);
  GlitchyChip chip(clock);
  Cc1101 drv(chip, clock);
  chip.garbageReads = 3;
  CHECK(drv.marcState() == kMarcIdle);
}

TEST_CASE("driver: PATABLE write overwrites the other seven entries with zero") {
  KRig r;
  for (int i = 0; i < 8; ++i) r.chip.patable[i] = 0xFF;
  r.drv.writePatable(0x02);
  CHECK(r.chip.patable[0] == 0x02);
  for (int i = 1; i < 8; ++i) CHECK(r.chip.patable[i] == 0x00);
}

TEST_CASE("driver: reset holds chip select high for the datasheet's 40 us measured from the low edge") {
  sim::FakeClock clock(500);
  TimedChip chip(clock);
  Cc1101 drv(chip, clock);
  REQUIRE(drv.reset());
  REQUIRE(chip.selects.size() >= 4);
  // [0] CSN high, [1] CSN low, [2] CSN high, [3] CSN low again: low edge to next low edge must be >= 40 us
  CHECK_FALSE(chip.selects[0].first);
  CHECK(chip.selects[1].first);
  CHECK_FALSE(chip.selects[2].first);
  CHECK(chip.selects[3].first);
  CHECK(chip.selects[3].second - chip.selects[1].second >= 40);
}

TEST_CASE("driver: apply drops the chip to idle before it touches a register") {
  KRig r;
  r.drv.reset();
  r.drv.apply(profileProven(), 0x01);
  REQUIRE(r.drv.enterRx());
  r.chip.strobes.clear();
  CHECK(r.drv.apply(profileProven(), 0x01).ok);
  REQUIRE_FALSE(r.chip.strobes.empty());
  CHECK(r.chip.strobes.front().cmd == SIDLE);
  CHECK(r.chip.marc() == kMarcIdle);
}

TEST_CASE("device: begin() fails, and the device is not present, for each way the chip can be wrong") {
  {
    Bench b;  // not a CC1101
    b.chip.version = 0x77;
    CHECK_FALSE(b.boot());
    CHECK_FALSE(b.device->present());
  }
  {
    Bench b;  // a register that will not take its value
    b.chip.stuckReg[MDMCFG3] = true;
    CHECK_FALSE(b.boot());
    CHECK_FALSE(b.device->present());
  }
  {
    Bench b;  // a chip that never reaches receive
    b.chip.freezeState = true;
    CHECK_FALSE(b.boot());
    CHECK_FALSE(b.device->present());
  }
  {
    Bench b;  // a chip that never answers
    b.chip.stuckBusy = true;
    CHECK_FALSE(b.boot());
    CHECK_FALSE(b.device->present());
  }
}

TEST_CASE("device: setPatable refuses unknown bytes, steps above the cap and a failed read-back, and changes nothing") {
  Bench b;
  REQUIRE(b.boot());
  CHECK_FALSE(b.device->setPatable(0x77));  // not in the table
  CHECK_FALSE(b.device->setPatable(0xC0));  // above the cap
  CHECK(b.chip.patable[0] == 0x01);
  CHECK(b.device->patable() == 0x01);
  b.chip.readBackWrongPatable = true;
  CHECK_FALSE(b.device->setPatable(0x02));
  CHECK(b.device->patable() == 0x01);
}

TEST_CASE("device: begin() refuses an unknown PATABLE byte without touching the chip") {
  Bench b;
  CHECK_FALSE(b.boot(profileProven(), 0x77, 10.0));
  CHECK(b.chip.resets == 0);
}

TEST_CASE("device: while the transmitter is keyed nothing about the radio can be changed") {
  ModemConfig c;
  c.keyMode = KeyMode::kRts;
  Bench b(c);
  REQUIRE(b.boot());
  b.lines(true, true);
  b.run(10000);
  REQUIRE(b.modem->keyed());
  CHECK_FALSE(b.device->setPatable(0x02));
  CHECK(b.chip.patable[0] == 0x01);
  CHECK_FALSE(b.device->setProfile(profileHsduMatched()));
  CHECK(b.chip.reg[FREQ0] == 0xB1);
  SelfTestReport report;
  CHECK_FALSE(b.device->selfTest(report));
}

TEST_CASE("device: setProfile refuses when the radio is not present") {
  Bench b;
  b.chip.version = 0x77;
  REQUIRE_FALSE(b.boot());
  CHECK_FALSE(b.device->setProfile(profileHsduMatched()));
  CHECK(b.chip.reg[FREQ0] != 0xCF);
}

TEST_CASE("driver: when the chip does not reach idle, no RX or TX strobe is sent after the lost SIDLE") {
  KRig r;
  r.drv.reset();
  r.drv.apply(profileProven(), 0x01);
  REQUIRE(r.drv.enterTx());
  r.chip.strobes.clear();
  r.chip.ignoreNextStrobes = 1;
  CHECK_FALSE(r.drv.enterRx());
  REQUIRE(r.chip.strobes.size() == 1);
  CHECK(r.chip.strobes[0].cmd == SIDLE);

  KRig q;
  q.drv.reset();
  q.drv.apply(profileProven(), 0x01);
  REQUIRE(q.drv.enterRx());
  q.chip.strobes.clear();
  q.chip.ignoreNextStrobes = 1;
  CHECK_FALSE(q.drv.enterTx());
  REQUIRE(q.chip.strobes.size() == 1);
  CHECK(q.chip.strobes[0].cmd == SIDLE);
}

TEST_CASE("math: freqWordForHz rounds to the nearest word, not down") {
  const uint32_t w = 0x1013B1;
  const double step = kXtalHz / 65536.0;
  CHECK(freqWordForHz(carrierHz(w) + 0.6 * step) == w + 1);
  CHECK(freqWordForHz(carrierHz(w) + 0.4 * step) == w);
}

TEST_CASE("math: the data-rate encoder can pick mantissa 255 and the deviation encoder exponent 7") {
  const DataRateRegs d = encodeDataRate(dataRateBaud(0x07, 0xFF));
  CHECK(d.ok);
  CHECK(d.exponent == 7);
  CHECK(d.mantissa == 255);
  CHECK(encodeDeviation(deviationHz(0x77)) == 0x77);
}

TEST_CASE("device: setProfile reports a failed apply or a failed return to receive, and keeps the old profile") {
  {
    Bench b;
    REQUIRE(b.boot());
    b.chip.stuckReg[FREQ0] = true;  // proven wants 0xB1, matched wants 0xCF
    CHECK_FALSE(b.device->setProfile(profileHsduMatched()));
    CHECK(&b.device->profile() == &profileProven());
  }
  {
    Bench b;
    REQUIRE(b.boot());
    b.chip.freezeState = true;  // the registers take, but the chip never gets back to receive
    CHECK_FALSE(b.device->setProfile(profileHsduMatched()));
  }
  {
    Bench b;
    REQUIRE(b.boot());
    CHECK(b.device->setProfile(profileHsduMatched()));
    CHECK(&b.device->profile() == &profileHsduMatched());
  }
}

TEST_CASE("device: selftest notices a PATABLE that drifted and a chip that is not receiving") {
  {
    Bench b;
    REQUIRE(b.boot());
    b.chip.patable[0] = 0x02;  // something changed the output power behind the firmware's back
    SelfTestReport r;
    REQUIRE(b.device->selfTest(r));
    CHECK_FALSE(r.patableOk);
    CHECK_FALSE(r.pass);
  }
  {
    Bench b;
    REQUIRE(b.boot());
    REQUIRE(b.device->radio().enterIdle());
    SelfTestReport r;
    REQUIRE(b.device->selfTest(r));
    CHECK_FALSE(r.inRx);
    CHECK_FALSE(r.pass);
  }
}

TEST_CASE("ring buffer: capacity() reports the size") {
  RingBuffer<4> r;
  CHECK(r.capacity() == 4);
  CHECK(r.free() == 4);
}

TEST_CASE("device: a refused second begin() leaves the device not present") {
  Bench b;
  REQUIRE(b.boot());
  REQUIRE(b.device->present());
  CHECK_FALSE(b.boot(profileProven(), 0xC0, kDefaultMaxChipDbm));
  CHECK_FALSE(b.device->present());
}

}  // TEST_SUITE
