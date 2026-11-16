// Tests that kill the console / power / profile mutants the existing suite lets through. Each one passes on the
// unmodified code.
#include <map>
#include <string>

#include "bench.h"
#include "doctest.h"
#include "openhsdu/cc1101_regs.h"
#include "openhsdu/console.h"
#include "openhsdu/power.h"
#include "openhsdu/profiles.h"

using namespace openhsdu;
using sim::Bench;
using sim::FakeConsoleOut;

namespace {
struct CRig {
  Bench b;
  FakeConsoleOut out;
  Console con;
  CRig() : b(), con(*b.device, *b.modem, out) {
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

TEST_SUITE("mutation-kills") {

TEST_CASE("console: the limits themselves are accepted (carrier 1000 ms, pattern 64 bytes)") {
  CRig r;
  CHECK(r.say("carrier 1000").find("ok carrier 1000 ms") != std::string::npos);
  r.b.run(1500000);
  REQUIRE_FALSE(r.b.modem->keyed());
  CHECK(r.say("pattern 64").find("ok pattern 64 bytes") != std::string::npos);
}

TEST_CASE("console: pattern with no argument sends 16 bytes") {
  CRig r;
  CHECK(r.say("pattern").find("ok pattern 16 bytes") != std::string::npos);
}

TEST_CASE("console: carrier 0 is a usage error, not a 'busy' error") {
  CRig r;
  const std::string s = r.say("carrier 0");
  CHECK(s.find("carrier [ms 1-1000]") != std::string::npos);
  CHECK(s.find("busy") == std::string::npos);
}

TEST_CASE("console: a line of exactly 80 characters is accepted, 81 is refused") {
  CRig r;
  CHECK(r.say(std::string(80, 'x').c_str()).find("unknown command") != std::string::npos);
  const std::string s = r.say(std::string(81, 'x').c_str());
  CHECK(s.find("line too long") != std::string::npos);
  CHECK(s.find("unknown command") == std::string::npos);
}

TEST_CASE("console: DEL (0x7F) erases the previous character like backspace") {
  CRig r;
  r.out.clear();
  r.con.feedString("vex\x7f" "r\n");
  CHECK(r.out.has("open-hsdu"));
}

TEST_CASE("console: a hex number that overflows 32 bits is an error, not a wrapped value") {
  CRig r;
  CHECK(r.say("reg 100000008").find("error") != std::string::npos);  // would wrap to 0x08 without the check
  CHECK(r.say("power 100000002").find("error") != std::string::npos);
  CHECK(r.b.device->patable() == 0x01);
}

TEST_CASE("console: with the radio missing, carrier and pattern are refused and nothing is queued") {
  Bench b;
  b.chip.version = 0x77;  // not a CC1101
  REQUIRE_FALSE(b.boot());
  FakeConsoleOut out;
  Console con(*b.device, *b.modem, out);
  con.feedString("carrier 100\n");
  CHECK(out.has("busy or radio missing"));
  out.clear();
  con.feedString("pattern 8\n");
  CHECK(out.has("busy or radio missing"));
  b.run(300000);
  CHECK(b.uart.sent.empty());
}

TEST_CASE("profiles: registers the other tests only compare between the two profiles are pinned to their documented values") {
  // SmartRF Studio values for 433 MHz / 4.8 kbaud / 2-FSK, and MCSM1 = 0x00 (CCA off, return to idle after RX and TX).
  const std::map<uint8_t, uint8_t> pinned = {{cc1101::FSCAL3, 0xE9}, {cc1101::FSCAL2, 0x2A}, {cc1101::FSCAL1, 0x00},
                                             {cc1101::FSCAL0, 0x1F}, {cc1101::TEST2, 0x81},  {cc1101::TEST1, 0x35},
                                             {cc1101::TEST0, 0x09},  {cc1101::MCSM1, 0x00}};
  for (const Profile* p : {&profileProven(), &profileHsduMatched()}) {
    std::map<uint8_t, uint8_t> m;
    for (size_t i = 0; i < p->count; ++i) m[p->regs[i].addr] = p->regs[i].value;
    for (const auto& kv : pinned) {
      INFO(p->name << " register 0x" << std::hex << int(kv.first));
      REQUIRE(m.count(kv.first) == 1);
      CHECK(m.at(kv.first) == kv.second);
    }
  }
}

TEST_CASE("power table: the allowed steps are pinned to their datasheet values, and the margin test uses the table") {
  // The existing margin test uses a literal -33.8; this one uses what the firmware will actually enforce.
  const double limit = part15_209_limitEirpDbm();
  for (int step : {0x01, 0x02, 0x11}) {
    const PowerStep* s = findPowerStep(static_cast<uint8_t>(step));
    REQUIRE(s != nullptr);
    INFO("step 0x" << std::hex << step);
    CHECK(s->chipDbm <= -33.8 + 1e-9);
    const EirpBudget b{s->chipDbm, 20.0, 2.0, 0.0};
    CHECK(limit - b.eirpDbm() >= 2.6 - 0.05);
  }
  CHECK(findPowerStep(0x02)->chipDbm == doctest::Approx(-33.8));
  CHECK(findPowerStep(0x11)->chipDbm == doctest::Approx(-33.8));
}

}  // TEST_SUITE
