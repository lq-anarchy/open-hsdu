#include "openhsdu/console.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "openhsdu/cc1101_math.h"
#include "openhsdu/cc1101_regs.h"
#include "openhsdu/power.h"
#include "openhsdu/version.h"

namespace openhsdu {

using namespace cc1101;

namespace {

const char* keyModeName(KeyMode m) {
  switch (m) {
    case KeyMode::kRts: return "rts";
    case KeyMode::kData: return "data";
    default: return "either";
  }
}

const char* stateName(Modem::State s) {
  switch (s) {
    case Modem::State::kRx: return "rx";
    case Modem::State::kTxLead: return "tx-lead";
    case Modem::State::kTxData: return "tx-data";
    default: return "tx-tail";
  }
}

// Parses a whole token as an unsigned number. Rejects empty text, trailing junk and overflow.
bool parseUnsigned(const char* s, int base, uint32_t* out) {
  if (s == nullptr || *s == '\0') return false;
  if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  if (*s == '\0') return false;
  uint64_t v = 0;
  for (; *s != '\0'; ++s) {
    int d;
    if (*s >= '0' && *s <= '9') d = *s - '0';
    else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
    else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
    else return false;
    if (d >= base) return false;
    v = v * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
    if (v > 0xFFFFFFFFull) return false;
  }
  *out = static_cast<uint32_t>(v);
  return true;
}

}  // namespace

void Console::print(const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (static_cast<size_t>(n) >= sizeof(buf)) n = static_cast<int>(sizeof(buf)) - 1;
  out_.write(buf, static_cast<size_t>(n));
}

void Console::feed(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const char c = static_cast<char>(data[i]);
    if (c == '\r' || c == '\n') {
      if (overflow_) {
        print("error: line too long or not text\n");
      } else if (len_ > 0) {
        line_[len_] = '\0';
        execute(line_);
      }
      len_ = 0;
      overflow_ = false;
    } else if (c == '\b' || c == 0x7F) {
      if (len_ > 0 && !overflow_) --len_;
    } else if (c == '\0') {
      overflow_ = true;  // a NUL would silently cut the line short for the string handling below: refuse the line
    } else if (!overflow_) {
      if (len_ >= kMaxLine) overflow_ = true;
      else line_[len_++] = c;
    }
  }
}

void Console::feedString(const char* text) {
  feed(reinterpret_cast<const uint8_t*>(text), std::strlen(text));
}

void Console::execute(char* line) {
  while (*line == ' ' || *line == '\t') ++line;
  char* cmd = line;
  while (*line != '\0' && *line != ' ' && *line != '\t') {
    *line = static_cast<char>(std::tolower(static_cast<unsigned char>(*line)));
    ++line;
  }
  char* arg = line;
  if (*arg != '\0') {
    *arg++ = '\0';
    while (*arg == ' ' || *arg == '\t') ++arg;
    char* end = arg + std::strlen(arg);
    while (end > arg && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
  }

  if (*cmd == '\0') return;
  if (std::strcmp(cmd, "help") == 0 || std::strcmp(cmd, "?") == 0) cmdHelp();
  else if (std::strcmp(cmd, "ver") == 0 || std::strcmp(cmd, "version") == 0) cmdVersion();
  else if (std::strcmp(cmd, "status") == 0) cmdStatus();
  else if (std::strcmp(cmd, "selftest") == 0) cmdSelfTest();
  else if (std::strcmp(cmd, "rssi") == 0) cmdRssi();
  else if (std::strcmp(cmd, "reg") == 0) cmdReg(arg);
  else if (std::strcmp(cmd, "regs") == 0) cmdRegs();
  else if (std::strcmp(cmd, "power") == 0) cmdPower(arg);
  else if (std::strcmp(cmd, "powers") == 0) cmdPowers();
  else if (std::strcmp(cmd, "profile") == 0) cmdProfile(arg);
  else if (std::strcmp(cmd, "keymode") == 0) cmdKeyMode(arg);
  else if (std::strcmp(cmd, "carrier") == 0) cmdCarrier(arg);
  else if (std::strcmp(cmd, "pattern") == 0) cmdPattern(arg);
  else print("error: unknown command '%s' (try help)\n", cmd);
}

void Console::cmdHelp() {
  print("ver | status | selftest | rssi | reg <hex> | regs\n");
  print("power [hex] | powers | profile [name] | keymode [rts|data|either]\n");
  print("carrier [ms] (max %u) | pattern [bytes] (0xAA, max %u)\n",
        static_cast<unsigned>(kMaxCarrierMs), static_cast<unsigned>(kMaxPatternBytes));
}

void Console::cmdVersion() {
  print("open-hsdu %s profile=%s keymode=%s\n", OPENHSDU_VERSION, dev_.profile().name, keyModeName(modem_.keyMode()));
}

void Console::cmdStatus() {
  const Modem::Stats& s = modem_.stats();
  print("state=%s dtr=%d rts=%d lockout=%d keymode=%s\n", stateName(modem_.state()), modem_.dtr() ? 1 : 0,
        modem_.rts() ? 1 : 0, modem_.lockedOut() ? 1 : 0, keyModeName(modem_.keyMode()));
  print("radio=%s profile=%s patable=0x%02X\n", dev_.present() ? "present" : "MISSING", dev_.profile().name,
        dev_.patable());
  print("tx=%u rx=%u keyups=%u watchdog=%u faults=%u unexpected-tx=%u\n", static_cast<unsigned>(s.txBytes),
        static_cast<unsigned>(s.rxBytes), static_cast<unsigned>(s.keyUps), static_cast<unsigned>(s.watchdogTrips),
        static_cast<unsigned>(s.radioFaults), static_cast<unsigned>(s.unexpectedTx));
  print("dropped: nokey=%u full=%u fault=%u aborted=%u nodtr=%u rx-while-tx=%u rx-settle=%u\n",
        static_cast<unsigned>(s.txDroppedNoKey), static_cast<unsigned>(s.txDroppedFull),
        static_cast<unsigned>(s.txDroppedFault), static_cast<unsigned>(s.txAborted), static_cast<unsigned>(s.hostDroppedNoDtr),
        static_cast<unsigned>(s.rxDiscardedTx), static_cast<unsigned>(s.rxDiscardedSettle));
}

void Console::cmdSelfTest() {
  SelfTestReport r;
  if (!dev_.selfTest(r)) {
    print("error: transmitting\n");
    return;
  }
  print("part=0x%02X version=0x%02X %s\n", r.part, r.version, r.detected ? "detected" : "NOT A CC1101");
  if (r.detected) {
    if (r.mismatches == 0) print("registers: all match profile %s\n", dev_.profile().name);
    else print("registers: %u differ, first at 0x%02X\n", r.mismatches, r.firstBadAddr);
    print("patable: %s\n", r.patableOk ? "ok" : "MISMATCH");
    print("marcstate=0x%02X %s\n", r.marc, r.inRx ? "(receiving)" : "(not receiving)");
  }
  print("%s\n", r.pass ? "PASS" : "FAIL");
}

void Console::cmdRssi() {
  if (!dev_.present()) {
    print("error: radio missing\n");
    return;
  }
  if (modem_.keyed()) {
    print("error: transmitting\n");
    return;
  }
  // The chip must be receiving; the offset (74 dB at 433 MHz) is the datasheet's typical value, so the number is
  // approximate. Use it to compare signals, not to measure them.
  const uint8_t raw = dev_.radio().readStatus(RSSI);
  print("rssi raw=0x%02X approx %.1f dBm\n", raw, Cc1101::rssiDbmFromRaw(raw));
}

void Console::cmdReg(char* arg) {
  uint32_t a = 0;
  if (!parseUnsigned(arg, 16, &a) || a >= kNumConfigRegs) {
    print("error: reg <hex address 00-2E>\n");
    return;
  }
  if (modem_.keyed()) {
    print("error: transmitting\n");
    return;
  }
  print("reg 0x%02X = 0x%02X\n", static_cast<unsigned>(a), dev_.radio().readReg(static_cast<uint8_t>(a)));
}

void Console::cmdRegs() {
  if (modem_.keyed()) {
    print("error: transmitting\n");
    return;
  }
  for (uint8_t row = 0; row < kNumConfigRegs; row += 8) {
    char buf[64];
    int n = std::snprintf(buf, sizeof(buf), "%02X:", row);
    for (uint8_t i = row; i < row + 8 && i < kNumConfigRegs; ++i)
      n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %02X", dev_.radio().readReg(i));
    n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), "\n");
    out_.write(buf, static_cast<size_t>(n));
  }
}

void Console::cmdPower(char* arg) {
  if (*arg == '\0') {
    const PowerStep* s = findPowerStep(dev_.patable());
    print("power patable=0x%02X chip=%.1f dBm cap=%.1f dBm\n", dev_.patable(), s != nullptr ? s->chipDbm : 0.0,
          dev_.maxChipDbm());
    return;
  }
  uint32_t v = 0;
  if (!parseUnsigned(arg, 16, &v) || v > 0xFF) {
    print("error: power <hex byte>\n");
    return;
  }
  const PowerStep* s = findPowerStep(static_cast<uint8_t>(v));
  if (s == nullptr) {
    print("error: 0x%02X is not an allowed step (see powers)\n", static_cast<unsigned>(v));
  } else if (s->chipDbm > dev_.maxChipDbm()) {
    print("error: 0x%02X is %.1f dBm, above this build's cap of %.1f dBm\n", static_cast<unsigned>(v), s->chipDbm,
          dev_.maxChipDbm());
  } else if (modem_.keyed()) {
    print("error: transmitting\n");
  } else if (!dev_.setPatable(static_cast<uint8_t>(v))) {
    print("error: radio did not accept it\n");
  } else {
    print("ok power 0x%02X chip %.1f dBm\n", static_cast<unsigned>(v), s->chipDbm);
  }
}

void Console::cmdPowers() {
  size_t n = 0;
  const PowerStep* steps = powerSteps(&n);
  for (size_t i = 0; i < n; ++i)
    print("0x%02X %5.1f dBm %s\n", steps[i].patable, steps[i].chipDbm,
          steps[i].chipDbm <= dev_.maxChipDbm() ? "allowed" : "above cap");
}

void Console::cmdProfile(char* arg) {
  if (*arg == '\0') {
    print("profile %s\n", dev_.profile().name);
    return;
  }
  for (char* p = arg; *p != '\0'; ++p) *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
  const Profile* p = profileByName(arg);
  if (p == nullptr) print("error: profiles are proven, matched\n");
  else if (modem_.keyed()) print("error: transmitting\n");
  else if (!dev_.setProfile(*p)) print("error: radio did not accept it\n");
  else print("ok profile %s\n", p->name);
}

void Console::cmdKeyMode(char* arg) {
  if (*arg == '\0') {
    print("keymode %s\n", keyModeName(modem_.keyMode()));
    return;
  }
  for (char* p = arg; *p != '\0'; ++p) *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
  if (std::strcmp(arg, "rts") == 0) modem_.setKeyMode(KeyMode::kRts);
  else if (std::strcmp(arg, "data") == 0) modem_.setKeyMode(KeyMode::kData);
  else if (std::strcmp(arg, "either") == 0) modem_.setKeyMode(KeyMode::kEither);
  else {
    print("error: keymode rts|data|either\n");
    return;
  }
  print("ok keymode %s\n", keyModeName(modem_.keyMode()));
}

void Console::cmdCarrier(char* arg) {
  uint32_t ms = 500;
  if (*arg != '\0' && (!parseUnsigned(arg, 10, &ms) || ms == 0)) {
    print("error: carrier [ms 1-%u]\n", static_cast<unsigned>(kMaxCarrierMs));
    return;
  }
  if (ms > kMaxCarrierMs) {
    print("error: at most %u ms\n", static_cast<unsigned>(kMaxCarrierMs));
    return;
  }
  if (!dev_.present() || !modem_.carrierBurst(ms * 1000u)) print("error: busy or radio missing\n");
  else print("ok carrier %u ms\n", static_cast<unsigned>(ms));
}

void Console::cmdPattern(char* arg) {
  uint32_t n = 16;
  if (*arg != '\0' && (!parseUnsigned(arg, 10, &n) || n == 0)) {
    print("error: pattern [bytes 1-%u]\n", static_cast<unsigned>(kMaxPatternBytes));
    return;
  }
  if (n > kMaxPatternBytes) {
    print("error: at most %u bytes\n", static_cast<unsigned>(kMaxPatternBytes));
    return;
  }
  uint8_t buf[kMaxPatternBytes];
  std::memset(buf, 0xAA, sizeof(buf));
  if (!dev_.present() || !modem_.sendBytes(buf, n)) print("error: busy or radio missing\n");
  else print("ok pattern %u bytes\n", static_cast<unsigned>(n));
}

}  // namespace openhsdu
