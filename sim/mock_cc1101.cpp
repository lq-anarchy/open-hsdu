#include "mock_cc1101.h"

#include <cstring>

namespace sim {

using namespace openhsdu::cc1101;

namespace {
// Reset values from the datasheet's register overview.
constexpr uint8_t kResetValues[kNumConfigRegs] = {
    0x29, 0x2E, 0x3F, 0x07, 0xD3, 0x91, 0xFF, 0x04, 0x45, 0x00, 0x00, 0x0F, 0x00, 0x1E, 0xC4, 0xEC,
    0x8C, 0x22, 0x02, 0x22, 0xF8, 0x47, 0x07, 0x30, 0x04, 0x36, 0x6C, 0x03, 0x40, 0x91, 0x87, 0x6B,
    0xF8, 0x56, 0x10, 0xA9, 0x0A, 0x20, 0x0D, 0x41, 0x00, 0x59, 0x7F, 0x3F, 0x88, 0x31, 0x0B};
}  // namespace

MockCc1101::MockCc1101(FakeClock& clock) : clock_(clock) {
  resetRegisters();
  history.push_back({clock_.micros(), kMarcIdle});
}

void MockCc1101::resetRegisters() {
  std::memcpy(reg, kResetValues, sizeof(reg));
  std::memset(patable, 0, sizeof(patable));
  patable[0] = 0xC6;  // datasheet reset value
  patableIdx_ = 0;
}

void MockCc1101::commitState(uint32_t t, uint8_t marc) {
  marc_ = marc;
  history.push_back({t, marc});
}

void MockCc1101::resolve() {
  if (pending_ && static_cast<int32_t>(clock_.micros() - pendingAt_) >= 0) {
    commitState(pendingAt_, pendingMarc_);
    pending_ = false;
    if (pendingCalibrated_) {
      // A real chip writes its calibration RESULTS into FSCAL3[3:0], FSCAL2[4:0] and FSCAL1[5:0]: values that
      // differ from what the profile wrote, so a read-back that compares them fails, as it would on silicon.
      reg[FSCAL3] = static_cast<uint8_t>((reg[FSCAL3] & 0xF0) | 0x07);
      reg[FSCAL2] = static_cast<uint8_t>((reg[FSCAL2] & 0xE0) | 0x0C);
      reg[FSCAL1] = 0x15;
      ++calibrations;
    }
  }
}

uint8_t MockCc1101::marc() {
  resolve();
  return pending_ ? static_cast<uint8_t>(kMarcStartcal) : marc_;
}

uint8_t MockCc1101::stateAt(uint32_t t) const {
  uint8_t s = history.empty() ? static_cast<uint8_t>(kMarcIdle) : history.front().marc;
  for (const StateChange& c : history) {
    if (static_cast<int32_t>(t - c.t) >= 0) s = c.marc;
    else break;
  }
  return s;
}

void MockCc1101::strobe(uint8_t cmd) {
  resolve();
  strobes.push_back({clock_.micros(), cmd});
  if (cmd == SRES) {
    ++resets;
    resetRegisters();
    pending_ = false;
    commitState(clock_.micros(), kMarcIdle);
    return;
  }
  const bool changes = cmd == SIDLE || cmd == STX || cmd == SRX;
  if (!changes || freezeState) return;
  if (cmd == STX && loseTx) return;
  ++changing_;
  if (dropCount_ > 0 && changing_ > dropFrom_) {
    --dropCount_;
    return;
  }
  if (ignoreNextStrobes > 0) {
    --ignoreNextStrobes;
    return;
  }
  if (cmd == SIDLE) {
    pending_ = false;
    commitState(clock_.micros(), kMarcIdle);
    return;
  }
  if (marc_ != kMarcIdle || pending_) return;  // the driver always goes through idle first
  const bool autocal = (reg[MCSM0] & 0x30) == 0x10;
  pending_ = true;
  pendingCalibrated_ = autocal;
  pendingMarc_ = cmd == STX ? kMarcTx : kMarcRx;
  pendingAt_ = clock_.micros() + (autocal ? calUs : fastUs);
}

uint8_t MockCc1101::statusByte() {
  // Bit 7 = chip not ready (never here); bits 6:4 = state; bits 3:0 = FIFO bytes.
  const uint8_t m = marc();
  uint8_t state = 0;  // IDLE
  if (m == kMarcRx) state = 1;
  else if (m == kMarcTx) state = 2;
  else if (pending_) state = 4;  // calibrate (datasheet table: 4 = CALIBRATE, 5 = SETTLING)
  return static_cast<uint8_t>(state << 4);
}

uint8_t MockCc1101::readStatusReg(uint8_t addr) {
  switch (addr) {
    case PARTNUM: return partnum;
    case VERSION: return version;
    case MARCSTATE: {
      if (glitchNextMarcRead) {
        glitchNextMarcRead = false;
        return 0x1F;
      }
      return marc();
    }
    default: return 0;
  }
}

void MockCc1101::select(bool active) {
  if (active) {
    ++selectCount;
    if (selected_) ++csViolations;
  }
  selected_ = active;
  byteIndex_ = 0;
}

uint8_t MockCc1101::transfer(uint8_t out) {
  if (!selected_) return 0xFF;
  if (byteIndex_ == 0) {
    hdr_ = out;
    headers.push_back(out);
    read_ = (out & kHeaderRead) != 0;
    burst_ = (out & kHeaderBurst) != 0;
    addr_ = out & 0x3F;
    byteIndex_ = 1;
    patableIdx_ = 0;
    if (addr_ >= 0x30 && addr_ <= 0x3D && !burst_) strobe(addr_);
    return statusByte();
  }
  ++byteIndex_;
  if (!read_) {
    if (addr_ == PATABLE) {
      patable[patableIdx_ & 7] = out;
      ++patableIdx_;
    } else if (addr_ < kNumConfigRegs) {
      if (!stuckReg[addr_]) reg[addr_] = out;
      if (burst_) ++addr_;
    }
    return statusByte();
  }
  uint8_t v = 0;
  if (addr_ == PATABLE) {
    v = patable[patableIdx_ & 7];
    ++patableIdx_;
    if (readBackWrongPatable) v ^= 0x01;
  } else if (addr_ >= 0x30 && burst_) {
    v = readStatusReg(addr_);
  } else if (addr_ < kNumConfigRegs) {
    v = reg[addr_];
    if (burst_) ++addr_;
  }
  return v;
}

}  // namespace sim
