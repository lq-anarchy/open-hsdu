// One USB serial port, two jobs: the HSDU data port, and (at 115200 baud) the text console.
#include "bench.h"
#include "doctest.h"
#include "openhsdu/host_mux.h"

using namespace openhsdu;
using sim::Bench;

namespace {

struct MuxRig {
  Bench b;
  HostMux mux;
  Console con;
  MuxRig(ModemConfig cfg = ModemConfig()) : b(cfg), mux(*b.modem, b.host), con(*b.device, *b.modem, mux) {
    mux.setConsole(&con);
    REQUIRE(b.boot());
  }
  void open(uint32_t baud, bool dtr = true, bool rts = false) {
    mux.onLineCoding(baud);
    mux.onLineState(dtr, rts);
  }
  void type(const char* text) {
    mux.onHostBytes(reinterpret_cast<const uint8_t*>(text), std::strlen(text));
    for (int i = 0; i < 50; ++i) {
      mux.poll();
      b.run(100);
    }
  }
  std::string heard() const { return std::string(b.host.received.begin(), b.host.received.end()); }
};

}  // namespace

TEST_SUITE("host_mux") {

TEST_CASE("exactly 115200 baud is the console; every other baud rate is the data port") {
  MuxRig r;
  for (uint32_t baud : {0u, 300u, 4800u, 9600u, 57600u, 115199u, 115201u, 921600u}) {
    r.mux.onLineCoding(baud);
    INFO("baud " << baud);
    CHECK_FALSE(r.mux.consoleMode());
  }
  r.mux.onLineCoding(115200);
  CHECK(r.mux.consoleMode());
}

TEST_CASE("data port: bytes go to the modem and the console never sees them") {
  MuxRig r;
  r.open(4800, true, true);
  r.b.run(5000);
  r.mux.onHostBytes(reinterpret_cast<const uint8_t*>("ver\n"), 4);  // text that would be a console command
  r.b.run(200000);
  CHECK(r.b.uart.bytes() == std::vector<uint8_t>{'v', 'e', 'r', '\n'});  // transmitted, not interpreted
  CHECK(r.heard().find("open-hsdu") == std::string::npos);
}

TEST_CASE("console: a command gets its answer back on the same port") {
  MuxRig r;
  r.open(115200);
  r.type("ver\n");
  CHECK(r.heard().find("open-hsdu") != std::string::npos);
}

TEST_CASE("console: opening the port at 115200 with RTS and DTR raised cannot key the transmitter") {
  MuxRig r;
  r.open(115200, true, true);  // a terminal program raises both lines when it opens the port
  r.b.run(500000);
  CHECK(r.b.txWindows() == 0);
  CHECK_FALSE(r.b.modem->keyed());
}

TEST_CASE("console: a terminal that raises RTS BEFORE it sets 115200 baud still cannot key the transmitter") {
  for (KeyMode mode : {KeyMode::kRts, KeyMode::kEither}) {
    ModemConfig cfg;
    cfg.keyMode = mode;
    MuxRig r(cfg);
    r.mux.onLineState(true, true);   // the port opens, RTS up, the old baud rate still in force
    r.b.run(2000);
    r.mux.onLineCoding(115200);      // the baud rate arrives a moment later
    r.b.run(300000);
    INFO("mode " << int(mode));
    CHECK(r.b.txWindows() == 0);
    CHECK_FALSE(r.b.modem->keyed());
    CHECK(r.mux.consoleMode());
  }
}

TEST_CASE("data port: a host that opens at 4800 with RTS and writes after its usual 80 ms is not affected by the hold-off") {
  ModemConfig cfg;
  cfg.keyMode = KeyMode::kRts;
  MuxRig r(cfg);
  r.mux.onLineCoding(4800);
  r.mux.onLineState(true, true);
  r.b.run(80000);  // the host waits 80 ms after raising RTS
  const auto wire = Bench::wireImage({0x31, 0x32, 0x33});
  r.mux.onHostBytes(wire.data(), wire.size());
  r.b.run(100000);
  r.mux.onLineState(true, false);
  r.b.run(20000);
  CHECK(r.b.uart.bytes() == wire);
}

TEST_CASE("console: a half-typed line is forgotten when the port closes or changes mode") {
  MuxRig r;
  r.open(115200);
  r.b.run(10000);
  r.mux.onHostBytes(reinterpret_cast<const uint8_t*>("carrier 1000"), 12);  // no newline
  r.mux.onLineState(false, false);                                          // the port closes
  r.mux.onLineState(true, false);                                           // a later session opens it again
  r.type("\n");                                                             // and presses Enter to wake it
  CHECK(r.b.txWindows() == 0);
  r.mux.onHostBytes(reinterpret_cast<const uint8_t*>("carrier 1000"), 12);
  r.mux.onLineCoding(4800);                                                 // mode switch
  r.mux.onLineCoding(115200);
  r.type("\n");
  CHECK(r.b.txWindows() == 0);
  CHECK_FALSE(r.b.modem->keyed());
}

TEST_CASE("console: bytes typed are never written to the modem, and radio noise never reaches the console") {
  MuxRig r;
  r.open(115200);
  r.b.run(10000);
  r.b.radioRx({0x90, 0x91, 0x92});  // the receiver has no squelch
  r.type("status\n");
  CHECK(r.b.uart.sent.empty());
  CHECK(r.heard().find("\x90") == std::string::npos);
  CHECK(r.heard().find("state=rx") != std::string::npos);
}

TEST_CASE("switching to the console while transmitting drops the carrier") {
  MuxRig r;
  r.open(4800, true, true);
  r.b.run(70000);  // RTS is not trusted for the first 50 ms after the port opens
  REQUIRE(r.b.modem->keyed());
  r.mux.onLineCoding(115200);
  r.b.run(20000);
  CHECK_FALSE(r.b.modem->keyed());
  CHECK(r.b.chip.marc() == cc1101::kMarcRx);
}

TEST_CASE("switching back to the data port restores the line state the PC had set") {
  MuxRig r;
  r.open(4800, true, true);
  r.b.run(70000);
  REQUIRE(r.b.modem->keyed());
  r.mux.onLineCoding(115200);
  r.b.run(20000);
  REQUIRE_FALSE(r.b.modem->keyed());
  r.mux.onLineCoding(4800);  // RTS is still held by the PC; it is trusted again after the hold-off
  r.b.run(70000);
  CHECK(r.b.modem->keyed());
  CHECK(r.b.modem->dtr());
  CHECK(r.b.modem->rts());
}

TEST_CASE("console text is delivered in full and in order even when the USB side takes a few bytes at a time") {
  MuxRig r;
  r.open(115200);
  r.b.host.maxPerWrite = 7;
  r.mux.onHostBytes(reinterpret_cast<const uint8_t*>("regs\n"), 5);
  for (int i = 0; i < 400; ++i) {
    r.mux.poll();
    r.b.run(100);
  }
  const std::string s = r.heard();
  CHECK(s.find("00: 0D 2E 2E 07 D3 91 FF 04") != std::string::npos);
  int rows = 0;
  for (char c : s) rows += c == '\n';
  CHECK(rows == 6);
}

TEST_CASE("console text is not delivered when the PC has not raised DTR, and is not kept for later") {
  MuxRig r;
  r.open(115200, false, false);
  r.mux.onHostBytes(reinterpret_cast<const uint8_t*>("ver\n"), 4);
  for (int i = 0; i < 20; ++i) {
    r.mux.poll();
    r.b.run(100);
  }
  CHECK(r.b.host.received.empty());
  r.mux.onLineState(true, false);
  for (int i = 0; i < 20; ++i) {
    r.mux.poll();
    r.b.run(100);
  }
  CHECK(r.b.host.received.empty());
}

TEST_CASE("queued console text from one mode is never delivered in the other") {
  MuxRig r;
  r.open(115200);
  r.b.host.blocked = true;
  r.mux.onHostBytes(reinterpret_cast<const uint8_t*>("regs\n"), 5);
  r.mux.poll();
  r.mux.onLineCoding(4800);
  r.b.host.blocked = false;
  for (int i = 0; i < 50; ++i) {
    r.mux.poll();
    r.b.run(100);
  }
  CHECK(r.b.host.received.empty());
}

TEST_CASE("a data-port host sees the real HSDU behaviour end to end through the mux") {
  ModemConfig cfg;
  cfg.keyMode = KeyMode::kRts;
  MuxRig r(cfg);
  r.open(4800, true, false);
  r.b.run(10000);
  r.mux.onLineState(true, true);
  r.b.run(80000);
  const auto wire = Bench::wireImage({0x31, 0x32, 0x33});
  r.mux.onHostBytes(wire.data(), wire.size());
  r.b.run(100000);
  r.mux.onLineState(true, false);
  r.b.run(20000);
  CHECK(r.b.uart.bytes() == wire);
  CHECK(r.b.host.received == wire);  // the echo
}

}  // TEST_SUITE
