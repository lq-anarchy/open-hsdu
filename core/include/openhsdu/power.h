// Transmit power: the steps this firmware will use, and the arithmetic that says what they radiate.
//
// The firmware refuses any PATABLE byte that is not in this table, and by default any step whose nominal chip
// output is above a conservative cap. Raising the cap is a build option meant for a dummy load or a cabled
// bench set-up, never an antenna. See docs/REGULATORY.md.
#pragma once
#include <cstddef>
#include <cstdint>

namespace openhsdu {

struct PowerStep {
  uint8_t patable;  // value written to PATABLE[0]
  double chipDbm;   // nominal output of the chip at ~433 MHz, typical (datasheet / TI DN013); not measured here
};

// All allowed steps, lowest power first.
const PowerStep* powerSteps(size_t* count);
const PowerStep* findPowerStep(uint8_t patable);  // nullptr if the byte is not an allowed step

// Default: the lowest usable step. Step 0x00 is excluded: its harmonics are stronger than its carrier.
constexpr uint8_t kDefaultPatable = 0x01;
// Default cap on the nominal chip output. With a 20 dB attenuator and a 2 dBi antenna this stays under the
// 47 CFR 15.209 general limit (see EirpBudget and the tests).
constexpr double kDefaultMaxChipDbm = -33.0;

// The step to start with at boot, given the one stored in flash (0x00 = nothing stored): the stored step if it is an
// allowed step within this build's cap, otherwise the default. So a board that goes back to a lower-capped build, or
// whose stored byte is damaged, boots at the lowest step instead of refusing to start its radio.
uint8_t bootPatable(uint8_t stored, double maxChipDbm);

// Radiated power estimate from the chip output, an inline attenuator, and the antenna. Nothing here sees
// radiation from the board or wiring, which is why the real level has to be measured once.
struct EirpBudget {
  double chipDbm;
  double attenuatorDb;
  double antennaDbi;
  double otherLossDb;
  double eirpDbm() const { return chipDbm - attenuatorDb - otherLossDb + antennaDbi; }
};

// 47 CFR 15.209: 200 microvolts/metre at 3 metres, as EIRP in dBm (about -49.2).
double part15_209_limitEirpDbm();

}  // namespace openhsdu
