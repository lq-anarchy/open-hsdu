// The CC1101's own equations, written once as code so that the tests can check every register value
// against the numbers measured on the link. See docs/THEORY.md, "The chip's equations".
//
// All of them depend on the crystal frequency f_xosc (26 MHz on every module this project uses).
#pragma once
#include <cstdint>

namespace openhsdu {
namespace cc1101 {

constexpr double kXtalHz = 26.0e6;

// Carrier: f = f_xosc / 2^16 * FREQ, FREQ = {FREQ2[5:0], FREQ1, FREQ0}.
constexpr uint32_t freqWord(uint8_t freq2, uint8_t freq1, uint8_t freq0) {
  return (static_cast<uint32_t>(freq2 & 0x3F) << 16) | (static_cast<uint32_t>(freq1) << 8) | freq0;
}
double carrierHz(uint32_t word, double xtalHz = kXtalHz);
uint32_t freqWordForHz(double hz, double xtalHz = kXtalHz);  // nearest word

// Data rate: R = (256 + DRATE_M) * 2^DRATE_E / 2^28 * f_xosc, DRATE_E = MDMCFG4[3:0], DRATE_M = MDMCFG3.
double dataRateBaud(uint8_t mdmcfg4, uint8_t mdmcfg3, double xtalHz = kXtalHz);
struct DataRateRegs {
  uint8_t exponent;  // goes in MDMCFG4[3:0]
  uint8_t mantissa;  // goes in MDMCFG3
  double actualBaud;
  bool ok;
};
DataRateRegs encodeDataRate(double baud, double xtalHz = kXtalHz);  // smallest error over all exponents

// Receiver channel filter: BW = f_xosc / (8 * (4 + CHANBW_M) * 2^CHANBW_E), E = MDMCFG4[7:6], M = MDMCFG4[5:4].
double chanBwHz(uint8_t mdmcfg4, double xtalHz = kXtalHz);
uint8_t encodeChanBw(double hz, double xtalHz = kXtalHz);  // returns bits 7:4 of MDMCFG4, nearest bandwidth

// Frequency deviation: f_dev = f_xosc / 2^17 * (8 + DEVIATION_M) * 2^DEVIATION_E, E = DEVIATN[6:4], M = DEVIATN[2:0].
double deviationHz(uint8_t deviatn, double xtalHz = kXtalHz);
uint8_t encodeDeviation(double hz, double xtalHz = kXtalHz);  // nearest deviation

// Frequency-offset compensation limit, set by FOCCFG[1:0]: 0 = off, 1 = BW/8, 2 = BW/4, 3 = BW/2.
double focLimitHz(uint8_t foccfg, double chanBw);

// FCC 47 CFR 15.209 style field strength to EIRP: E (V/m) measured at d metres gives P = (E * d)^2 / 30 watts.
double eirpDbmFromFieldUvPerM(double microvoltsPerMetre, double metres);
double wattsToDbm(double watts);

// Wrap-safe elapsed time on a free-running 32-bit microsecond counter. Returns 0 if `since` is in the future.
inline uint32_t elapsedUs(uint32_t now, uint32_t since) {
  const int32_t d = static_cast<int32_t>(now - since);
  return d < 0 ? 0u : static_cast<uint32_t>(d);
}

}  // namespace cc1101
}  // namespace openhsdu
