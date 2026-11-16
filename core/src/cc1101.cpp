#include "openhsdu/cc1101.h"

#include "openhsdu/cc1101_math.h"
#include "openhsdu/cc1101_regs.h"

namespace openhsdu {

using namespace cc1101;

namespace {
constexpr uint32_t kReadyTimeoutUs = 3000;      // crystal start-up after CSN low is far below this
constexpr uint32_t kIdleTimeoutUs = 2000;
constexpr uint32_t kCalibrateTimeoutUs = 6000;  // idle -> RX/TX with calibration is about 720 us
constexpr uint32_t kPollUs = 20;
}  // namespace

bool Cc1101::waitReady() {
  const uint32_t start = clock_.micros();
  while (!spi_.misoLow()) {
    if (elapsedUs(clock_.micros(), start) > kReadyTimeoutUs) {
      ++timeouts_;
      return false;
    }
    clock_.delayUs(kPollUs);
  }
  return true;
}

uint8_t Cc1101::strobe(uint8_t cmd) {
  spi_.select(true);
  if (!waitReady()) {
    spi_.select(false);
    return 0xFF;
  }
  const uint8_t status = spi_.transfer(cmd);
  spi_.select(false);
  return status;
}

void Cc1101::writeReg(uint8_t addr, uint8_t value) {
  if (addr >= kNumConfigRegs) return;  // 0x30..0x3D without the burst bit are command strobes, never data
  spi_.select(true);
  if (waitReady()) {
    spi_.transfer(addr & 0x3F);
    spi_.transfer(value);
  }
  spi_.select(false);
}

uint8_t Cc1101::readReg(uint8_t addr) {
  if (addr >= kNumConfigRegs) return 0xFF;
  uint8_t v = 0xFF;
  spi_.select(true);
  if (waitReady()) {
    spi_.transfer(static_cast<uint8_t>((addr & 0x3F) | kHeaderRead));
    v = spi_.transfer(0x00);
  }
  spi_.select(false);
  return v;
}

uint8_t Cc1101::readStatus(uint8_t addr) {
  // Status registers sit at 0x30..0x3D; the burst bit is what tells the chip "status register, not strobe".
  auto once = [&]() {
    uint8_t v = 0xFF;
    spi_.select(true);
    if (waitReady()) {
      spi_.transfer(static_cast<uint8_t>((addr & 0x3F) | kHeaderRead | kHeaderBurst));
      v = spi_.transfer(0x00);
    }
    spi_.select(false);
    return v;
  };
  uint8_t a = once();
  for (int i = 0; i < 4; ++i) {
    const uint8_t b = once();
    if (a == b) return a;
    a = b;
  }
  return a;
}

uint8_t Cc1101::marcState() { return readStatus(MARCSTATE) & 0x1F; }

double Cc1101::rssiDbmFromRaw(uint8_t raw) {
  const int signedRaw = raw >= 128 ? static_cast<int>(raw) - 256 : static_cast<int>(raw);
  return signedRaw / 2.0 - 74.0;
}

double Cc1101::rssiDbm() { return rssiDbmFromRaw(readStatus(RSSI)); }

void Cc1101::writePatable(uint8_t value) {
  spi_.select(true);
  if (waitReady()) {
    spi_.transfer(static_cast<uint8_t>(PATABLE | kHeaderBurst));
    spi_.transfer(value);
    for (int i = 1; i < 8; ++i) spi_.transfer(0x00);
  }
  spi_.select(false);
}

uint8_t Cc1101::readPatable0() {
  uint8_t v = 0xFF;
  spi_.select(true);
  if (waitReady()) {
    spi_.transfer(static_cast<uint8_t>(PATABLE | kHeaderRead | kHeaderBurst));
    v = spi_.transfer(0x00);
    for (int i = 1; i < 8; ++i) spi_.transfer(0x00);
  }
  spi_.select(false);
  return v;
}

bool Cc1101::reset() {
  // Manual power-on reset (datasheet "Power on start-up sequence"): CSN high, low, high; wait; CSN low; wait for
  // MISO low; SRES; wait for MISO low again; CSN high.
  spi_.select(false);
  clock_.delayUs(10);
  spi_.select(true);
  clock_.delayUs(10);
  spi_.select(false);
  clock_.delayUs(45);
  spi_.select(true);
  if (!waitReady()) {
    spi_.select(false);
    return false;
  }
  spi_.transfer(SRES);
  const bool ready = waitReady();
  spi_.select(false);
  clock_.delayUs(100);
  return ready;
}

bool Cc1101::detect(uint8_t* partNum, uint8_t* version) {
  const uint8_t p = readStatus(PARTNUM);
  const uint8_t v = readStatus(VERSION);
  if (partNum != nullptr) *partNum = p;
  if (version != nullptr) *version = v;
  return p == kPartNum && (v == kVersionCurrent || v == kVersionOld);
}

bool Cc1101::waitState(uint8_t marc, uint32_t timeoutUs) {
  const uint32_t start = clock_.micros();
  for (;;) {
    if (marcState() == marc) return true;
    if (elapsedUs(clock_.micros(), start) > timeoutUs) {
      ++timeouts_;
      return false;
    }
    clock_.delayUs(kPollUs);
  }
}

bool Cc1101::enterIdle() {
  strobe(SIDLE);
  return waitState(kMarcIdle, kIdleTimeoutUs);
}

bool Cc1101::enterTx() {
  if (!enterIdle()) return false;
  strobe(STX);
  return waitState(kMarcTx, kCalibrateTimeoutUs);
}

bool Cc1101::enterRx() {
  if (!enterIdle()) return false;
  strobe(SRX);
  return waitState(kMarcRx, kCalibrateTimeoutUs);
}

ApplyResult Cc1101::apply(const Profile& profile, uint8_t patable) {
  ApplyResult r;
  enterIdle();
  for (size_t i = 0; i < profile.count; ++i) writeReg(profile.regs[i].addr, profile.regs[i].value);
  writePatable(patable);
  for (size_t i = 0; i < profile.count; ++i) {
    if (readReg(profile.regs[i].addr) != profile.regs[i].value) {
      if (r.mismatches == 0) r.firstBadAddr = profile.regs[i].addr;
      if (r.mismatches < 255) ++r.mismatches;
    }
  }
  r.patableOk = readPatable0() == patable;
  r.ok = r.mismatches == 0 && r.patableOk;
  return r;
}

}  // namespace openhsdu
