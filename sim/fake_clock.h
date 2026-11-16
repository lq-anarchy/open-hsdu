// A clock the tests control. delayUs() advances it, exactly as a real busy-wait would let time pass.
#pragma once
#include <cstdint>

#include "openhsdu/hal.h"

namespace sim {

class FakeClock : public openhsdu::IClock {
 public:
  explicit FakeClock(uint32_t start = 0) : now_(start) {}
  uint32_t micros() override { return now_; }
  void delayUs(uint32_t us) override { now_ += us; }
  void advance(uint32_t us) { now_ += us; }

 private:
  uint32_t now_;
};

}  // namespace sim
