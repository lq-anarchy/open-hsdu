// A model of the CC1101 on the SPI side: register file, PATABLE, status registers, command strobes, and the
// state machine with its calibration delay. Enough fidelity to test the driver and the modem; no RF.
// Faults can be injected to test the error paths.
#pragma once
#include <cstdint>
#include <vector>

#include "fake_clock.h"
#include "openhsdu/cc1101_regs.h"
#include "openhsdu/hal.h"

namespace sim {

class MockCc1101 : public openhsdu::ISpi {
 public:
  explicit MockCc1101(FakeClock& clock);

  // ISpi
  void select(bool active) override;
  uint8_t transfer(uint8_t out) override;
  bool misoLow() override { return !stuckBusy; }

  // The state the chip is in now, after any finished transition.
  uint8_t marc();
  // The state the chip was in at time t (microseconds on the fake clock).
  uint8_t stateAt(uint32_t t) const;
  bool transmittingAt(uint32_t t) const { return stateAt(t) == openhsdu::cc1101::kMarcTx; }

  // Configuration visible to tests.
  uint8_t reg[openhsdu::cc1101::kNumConfigRegs] = {};
  uint8_t patable[8] = {};

  // Timing: idle -> RX/TX takes calUs when MCSM0 asks for calibration, otherwise fastUs.
  uint32_t calUs = 799;   // datasheet: idle -> RX/TX with calibration, 799 us in total
  uint32_t fastUs = 88;

  // Identity and fault injection.
  uint8_t partnum = openhsdu::cc1101::kPartNum;
  uint8_t version = openhsdu::cc1101::kVersionCurrent;
  bool stuckBusy = false;          // MISO never goes low: the chip is not answering
  bool freezeState = false;        // strobes are accepted but the state never changes
  bool loseTx = false;             // every STX is lost, everything else works (a key-up that always fails)
  int ignoreNextStrobes = 0;       // swallow this many state-changing strobes
  bool glitchNextMarcRead = false; // the next MARCSTATE read returns garbage once
  // Lose `count` state-changing strobes after letting the next `skip` through (counted from now).
  void dropStrobes(int skip, int count) { dropFrom_ = changing_ + skip; dropCount_ = count; }
  bool stuckReg[openhsdu::cc1101::kNumConfigRegs] = {};  // writes to these are ignored (reads keep the old value)
  bool readBackWrongPatable = false;

  // Logs.
  struct StrobeLog {
    uint32_t t;
    uint8_t cmd;
  };
  struct StateChange {
    uint32_t t;
    uint8_t marc;
  };
  std::vector<StrobeLog> strobes;
  std::vector<StateChange> history;  // every committed state change, in time order
  std::vector<uint8_t> headers;      // every SPI header byte
  int resets = 0;
  int calibrations = 0;
  int selectCount = 0;
  int csViolations = 0;   // CSN taken low while it was already low (a transaction that never ended)
  bool selected() const { return selected_; }

 private:
  void resetRegisters();
  void strobe(uint8_t cmd);
  void commitState(uint32_t t, uint8_t marc);
  void resolve();
  uint8_t statusByte();
  uint8_t readStatusReg(uint8_t addr);

  FakeClock& clock_;
  uint8_t marc_ = openhsdu::cc1101::kMarcIdle;
  bool pending_ = false;
  bool pendingCalibrated_ = false;
  uint8_t pendingMarc_ = 0;
  uint32_t pendingAt_ = 0;

  int changing_ = 0;   // state-changing strobes seen so far
  int dropFrom_ = 0;
  int dropCount_ = 0;

  bool selected_ = false;
  int byteIndex_ = 0;
  uint8_t hdr_ = 0;
  uint8_t addr_ = 0;
  bool read_ = false;
  bool burst_ = false;
  int patableIdx_ = 0;
};

}  // namespace sim
