// The bring-up console: every command, its limits, and that it never lets the radio be misused.
#include "bench.h"
#include "doctest.h"
#include "openhsdu/console.h"
#include "openhsdu/version.h"

using namespace openhsdu;
using sim::Bench;
using sim::FakeConsoleOut;

namespace {
struct Rig {
  Bench b;
  FakeConsoleOut out;
  Console con;
  Rig() : b(), con(*b.device, *b.modem, out) {
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

TEST_SUITE("console") {

TEST_CASE("ver prints the version, profile and key mode") {
  Rig r;
  const std::string s = r.say("ver");
  CHECK(s.find(OPENHSDU_VERSION) != std::string::npos);
  CHECK(s.find("profile=proven") != std::string::npos);
  CHECK(s.find("keymode=either") != std::string::npos);
}

TEST_CASE("help lists the commands") {
  Rig r;
  const std::string s = r.say("help");
  for (const char* w : {"status", "selftest", "reg", "power", "profile", "keymode", "carrier", "pattern"})
    CHECK(s.find(w) != std::string::npos);
}

TEST_CASE("status shows state, lines, radio and counters") {
  Rig r;
  const std::string s = r.say("status");
  CHECK(s.find("state=rx") != std::string::npos);
  CHECK(s.find("dtr=1 rts=0") != std::string::npos);
  CHECK(s.find("radio=present") != std::string::npos);
  CHECK(s.find("patable=0x01") != std::string::npos);
  CHECK(s.find("tx=0 rx=0") != std::string::npos);
}

TEST_CASE("status reflects traffic") {
  Rig r;
  r.b.lines(true, true);
  r.b.pcWrite({1, 2, 3});
  r.b.run(50000);
  const std::string s = r.say("status");
  CHECK(s.find("tx=3") != std::string::npos);
  CHECK(s.find("keyups=1") != std::string::npos);
  CHECK(s.find("state=tx-") != std::string::npos);
}

TEST_CASE("status prints the boot line only when the firmware set one") {
  Rig r;
  CHECK(r.say("status").find("boot:") == std::string::npos);
  r.con.setBootInfo("reset=task-watchdog boots=7");
  CHECK(r.say("status").find("boot: reset=task-watchdog boots=7\n") != std::string::npos);
}

TEST_CASE("selftest passes on a healthy chip") {
  Rig r;
  const std::string s = r.say("selftest");
  CHECK(s.find("detected") != std::string::npos);
  CHECK(s.find("all match profile proven") != std::string::npos);
  CHECK(s.find("patable: ok") != std::string::npos);
  CHECK(s.find("PASS") != std::string::npos);
}

TEST_CASE("selftest fails, and says why, when a register has drifted") {
  Rig r;
  r.b.chip.reg[cc1101::MDMCFG3] = 0x22;
  const std::string s = r.say("selftest");
  CHECK(s.find("1 differ, first at 0x11") != std::string::npos);
  CHECK(s.find("FAIL") != std::string::npos);
}

TEST_CASE("selftest fails on something that is not a CC1101") {
  Rig r;
  r.b.chip.version = 0x77;
  const std::string s = r.say("selftest");
  CHECK(s.find("NOT A CC1101") != std::string::npos);
  CHECK(s.find("FAIL") != std::string::npos);
}

TEST_CASE("reg reads a register; bad addresses are refused") {
  Rig r;
  CHECK(r.say("reg 08").find("reg 0x08 = 0x32") != std::string::npos);
  CHECK(r.say("reg 0x12").find("= 0x04") != std::string::npos);
  CHECK(r.say("reg 2F").find("error") != std::string::npos);
  CHECK(r.say("reg zz").find("error") != std::string::npos);
  CHECK(r.say("reg").find("error") != std::string::npos);
  CHECK(r.say("reg 08 extra").find("error") != std::string::npos);
}

TEST_CASE("regs dumps all 47 configuration registers in rows of eight") {
  Rig r;
  const std::string s = r.say("regs");
  int rows = 0;
  for (char c : s) rows += c == '\n';
  CHECK(rows == 6);
  // IOCFG2 0x0D (profile), IOCFG1 0x2E (reset value), IOCFG0 0x2E (profile), FIFOTHR 0x07, SYNC1 0xD3,
  // SYNC0 0x91, PKTLEN 0xFF (reset values), PKTCTRL1 0x04 (profile)
  CHECK(s.find("00: 0D 2E 2E 07 D3 91 FF 04") != std::string::npos);
}

TEST_CASE("power shows the step; allowed steps can be chosen; others are refused") {
  Rig r;
  CHECK(r.say("power").find("patable=0x01 chip=-36.5") != std::string::npos);
  CHECK(r.say("power 02").find("ok power 0x02") != std::string::npos);
  CHECK(r.b.chip.patable[0] == 0x02);
  CHECK(r.b.device->patable() == 0x02);
  CHECK(r.say("power 11").find("ok") != std::string::npos);
  CHECK(r.say("power C0").find("above this build's cap") != std::string::npos);
  CHECK(r.say("power 12").find("above this build's cap") != std::string::npos);  // -30 dBm + 20 dB would break the limit
  CHECK(r.b.chip.patable[0] == 0x11);
  CHECK(r.say("power 33").find("not an allowed step") != std::string::npos);
  CHECK(r.say("power 00").find("not an allowed step") != std::string::npos);
  CHECK(r.say("power zz").find("error") != std::string::npos);
  CHECK(r.say("power 1FF").find("error") != std::string::npos);
  CHECK(r.b.chip.patable[0] == 0x11);  // nothing refused ever reached the chip
}

TEST_CASE("powers marks the three low steps allowed and the rest above the cap") {
  Rig r;
  const std::string s = r.say("powers");
  int allowed = 0, above = 0;
  size_t pos = 0;
  while ((pos = s.find("allowed", pos)) != std::string::npos) { ++allowed; pos += 7; }
  pos = 0;
  while ((pos = s.find("above cap", pos)) != std::string::npos) { ++above; pos += 9; }
  CHECK(allowed == 3);
  CHECK(above == 8);
}

TEST_CASE("a build with a higher cap can use a higher step; the default build cannot") {
  Bench b;
  REQUIRE(b.boot(profileProven(), 0x01, 10.0));
  FakeConsoleOut out;
  Console con(*b.device, *b.modem, out);
  con.feedString("power C0\n");
  CHECK(out.has("ok power 0xC0"));
  CHECK(b.chip.patable[0] == 0xC0);
}

TEST_CASE("the device refuses to boot above its cap") {
  Bench b;
  CHECK_FALSE(b.boot(profileProven(), 0xC0, kDefaultMaxChipDbm));
  CHECK_FALSE(b.device->present());
  CHECK(b.chip.patable[0] != 0xC0);
}

TEST_CASE("profile switches the register set; unknown names are refused") {
  Rig r;
  CHECK(r.say("profile").find("proven") != std::string::npos);
  CHECK(r.say("profile matched").find("ok profile matched") != std::string::npos);
  CHECK(r.b.chip.reg[cc1101::FREQ0] == 0xCF);
  CHECK(r.b.chip.reg[cc1101::DEVIATN] == 0x45);
  CHECK(r.b.chip.marc() == cc1101::kMarcRx);
  CHECK(r.say("profile bogus").find("error") != std::string::npos);
  CHECK(r.say("PROFILE Proven").find("ok profile proven") != std::string::npos);
  CHECK(r.b.chip.reg[cc1101::FREQ0] == 0xB1);
}

TEST_CASE("keymode") {
  Rig r;
  CHECK(r.say("keymode").find("either") != std::string::npos);
  CHECK(r.say("keymode rts").find("ok keymode rts") != std::string::npos);
  CHECK(r.b.modem->keyMode() == KeyMode::kRts);
  CHECK(r.say("keymode data").find("ok keymode data") != std::string::npos);
  CHECK(r.b.modem->keyMode() == KeyMode::kData);
  CHECK(r.say("keymode sideways").find("error") != std::string::npos);
  CHECK(r.b.modem->keyMode() == KeyMode::kData);
}

TEST_CASE("carrier keys a plain carrier for the time asked and no more than 1000 ms") {
  Rig r;
  CHECK(r.say("carrier 100").find("ok carrier 100 ms") != std::string::npos);
  r.b.run(20000);
  CHECK(r.b.modem->keyed());
  CHECK(r.b.uart.sent.empty());
  CHECK(r.say("carrier 100").find("error") != std::string::npos);  // already busy
  r.b.run(500000);
  CHECK_FALSE(r.b.modem->keyed());
  CHECK(r.say("carrier 1001").find("at most 1000") != std::string::npos);
  CHECK(r.say("carrier 0").find("error") != std::string::npos);
  CHECK(r.say("carrier -5").find("error") != std::string::npos);
  CHECK(r.say("carrier abc").find("error") != std::string::npos);
  CHECK_FALSE(r.b.modem->keyed());
}

TEST_CASE("carrier with no argument is 500 ms") {
  Rig r;
  CHECK(r.say("carrier").find("ok carrier 500 ms") != std::string::npos);
}

TEST_CASE("pattern sends a run of 0xAA and nothing else") {
  Rig r;
  CHECK(r.say("pattern 12").find("ok pattern 12 bytes") != std::string::npos);
  r.b.run(500000);
  CHECK(r.b.uart.bytes() == std::vector<uint8_t>(12, 0xAA));
  CHECK(r.say("pattern 65").find("at most 64") != std::string::npos);
  CHECK(r.say("pattern 0").find("error") != std::string::npos);
  CHECK(r.say("pattern x").find("error") != std::string::npos);
}

TEST_CASE("while the transmitter is keyed the console will not touch the chip") {
  Rig r;
  r.b.lines(true, true);
  r.b.run(10000);
  REQUIRE(r.b.modem->keyed());
  for (const char* cmd : {"reg 08", "regs", "selftest", "profile matched", "power 02"}) {
    INFO(cmd);
    CHECK(r.say(cmd).find("error: transmitting") != std::string::npos);
  }
  CHECK(r.b.chip.reg[cc1101::FREQ0] == 0xB1);
  CHECK(r.b.chip.patable[0] == 0x01);
}

TEST_CASE("loader: only the exact line `loader now` asks for the flash loader, and never while transmitting") {
  Rig r;
  CHECK(r.say("loader").find("error") != std::string::npos);
  CHECK(r.say("loader yes").find("error") != std::string::npos);
  CHECK(r.say("loader nowx").find("error") != std::string::npos);
  CHECK(r.say("loade now").find("unknown command") != std::string::npos);
  CHECK(r.say("status").find("ok loader") == std::string::npos);
  CHECK_FALSE(r.con.loaderRequested());

  r.b.lines(true, true);  // keyed: refuse, the carrier must not be left up by a restart
  r.b.run(5000);
  REQUIRE(r.b.modem->keyed());
  CHECK(r.say("loader now").find("error: transmitting") != std::string::npos);
  CHECK_FALSE(r.con.loaderRequested());
  r.b.lines(true, false);
  r.b.run(200000);
  REQUIRE_FALSE(r.b.modem->keyed());

  CHECK(r.say("LOADER NOW").find("error") != std::string::npos);  // the command ignores case, the argument does not
  CHECK_FALSE(r.con.loaderRequested());
}

TEST_CASE("loader now raises the request") {
  Rig r;
  CHECK(r.say("loader now") == "ok loader\n");
  CHECK(r.con.loaderRequested());
}

TEST_CASE("unknown commands and empty lines") {
  Rig r;
  CHECK(r.say("frobnicate").find("unknown command 'frobnicate'") != std::string::npos);
  CHECK(r.say("").empty());
  CHECK(r.say("   ").empty());
}

TEST_CASE("input handling: CR, LF and CRLF all end a line; backspace edits; case is ignored") {
  Rig r;
  r.out.clear();
  r.con.feedString("VER\r");
  CHECK(r.out.has("open-hsdu"));
  r.out.clear();
  r.con.feedString("vex\bR\r\n");
  CHECK(r.out.has("open-hsdu"));
  r.out.clear();
  r.con.feedString("ver\n\n\n");
  CHECK(r.out.has("open-hsdu"));
}

TEST_CASE("a line longer than the buffer is refused whole, and the next line works") {
  Rig r;
  r.out.clear();
  r.con.feedString(std::string(200, 'x').c_str());
  r.con.feedString("\n");
  CHECK(r.out.has("line too long"));
  r.out.clear();
  r.con.feedString("ver\n");
  CHECK(r.out.has("open-hsdu"));
}

TEST_CASE("a command split across feed calls is assembled") {
  Rig r;
  r.out.clear();
  r.con.feedString("st");
  r.con.feedString("at");
  r.con.feedString("us\n");
  CHECK(r.out.has("state=rx"));
}

TEST_CASE("the console cannot send arbitrary bytes: its only transmit commands are carrier and 0xAA") {
  Rig r;
  for (const char* cmd : {"send 31 32", "tx 41", "write 55", "pattern 55", "pattern 0xAA"}) r.say(cmd);
  r.b.run(500000);
  for (uint8_t v : r.b.uart.bytes()) CHECK(v == 0xAA);
}

}  // TEST_SUITE
