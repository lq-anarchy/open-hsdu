#include "openhsdu/power.h"

#include "openhsdu/cc1101_math.h"

namespace openhsdu {

namespace {
// Lowest first. 0x01, 0x02 and 0x11 are the low steps from TI application note DN013 (SWRA151A), typical values,
// never measured here (see docs/THEORY.md section 7). The rest are the datasheet's 433 MHz table.
constexpr PowerStep kSteps[] = {
    {0x01, -36.5}, {0x02, -33.8}, {0x11, -33.8}, {0x12, -30.0}, {0x0E, -20.0},
    {0x1D, -15.0}, {0x34, -10.0}, {0x60, 0.0},   {0x84, 5.0},   {0xC8, 7.0},
    {0xC0, 10.0},
};
}  // namespace

const PowerStep* powerSteps(size_t* count) {
  if (count != nullptr) *count = sizeof(kSteps) / sizeof(kSteps[0]);
  return kSteps;
}

const PowerStep* findPowerStep(uint8_t patable) {
  for (const PowerStep& s : kSteps) {
    if (s.patable == patable) return &s;
  }
  return nullptr;
}

double part15_209_limitEirpDbm() { return cc1101::eirpDbmFromFieldUvPerM(200.0, 3.0); }

}  // namespace openhsdu
