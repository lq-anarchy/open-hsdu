// Radio register profiles. A profile is a list of (address, value) writes applied after a chip reset; every
// register not listed keeps its reset value. Each profile also states its intent (carrier, baud, deviation,
// channel bandwidth) so the tests can decode the registers and compare.
#pragma once
#include <cstddef>
#include <cstdint>

namespace openhsdu {

enum class RegSource : uint8_t {
  kProven,      // value of the Flipper preset that was measured to work with real packs (docs/THEORY.md)
  kBareModule,  // changed or added for a bare module instead of the Flipper's board (not yet tried on hardware)
  kSmartRf      // TI's recommended value; the proven preset almost certainly sets it too, but it is not in the list
};

struct RegPair {
  uint8_t addr;
  uint8_t value;
  RegSource source;
};

struct Profile {
  const char* name;
  const RegPair* regs;
  size_t count;
  // What the registers are meant to produce, for the tests and for `status` output.
  double carrierHz;
  double baud;
  double deviationHz;
  double chanBwHz;
};

// 418.000 MHz, 4,798 baud, +/-47.6 kHz, 270.8 kHz filter: the preset that was proven on the packs.
const Profile& profileProven();
// 418.012 MHz and +/-41 kHz: the numbers measured on the HSDU's own signal. Not yet tried on a pack.
const Profile& profileHsduMatched();

// Look a profile up by name ("proven", "matched"); nullptr if unknown.
const Profile* profileByName(const char* name);

}  // namespace openhsdu
