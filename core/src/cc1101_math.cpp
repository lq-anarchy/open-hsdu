#include "openhsdu/cc1101_math.h"

#include <cmath>

namespace openhsdu {
namespace cc1101 {

double carrierHz(uint32_t word, double xtalHz) { return xtalHz / 65536.0 * static_cast<double>(word); }

uint32_t freqWordForHz(double hz, double xtalHz) {
  const double w = hz * 65536.0 / xtalHz;
  return static_cast<uint32_t>(std::llround(w));
}

double dataRateBaud(uint8_t mdmcfg4, uint8_t mdmcfg3, double xtalHz) {
  const int e = mdmcfg4 & 0x0F;
  return (256.0 + mdmcfg3) * std::ldexp(1.0, e) / std::ldexp(1.0, 28) * xtalHz;
}

DataRateRegs encodeDataRate(double baud, double xtalHz) {
  DataRateRegs best{0, 0, 0.0, false};
  double bestErr = 1e300;
  for (int e = 0; e <= 15; ++e) {
    const double m = baud * std::ldexp(1.0, 28) / (xtalHz * std::ldexp(1.0, e)) - 256.0;
    const long mi = std::lround(m);
    if (mi < 0 || mi > 255) continue;
    const double actual = dataRateBaud(static_cast<uint8_t>(e), static_cast<uint8_t>(mi), xtalHz);
    const double err = std::fabs(actual - baud);
    if (err < bestErr) {
      bestErr = err;
      best = {static_cast<uint8_t>(e), static_cast<uint8_t>(mi), actual, true};
    }
  }
  return best;
}

double chanBwHz(uint8_t mdmcfg4, double xtalHz) {
  const int e = (mdmcfg4 >> 6) & 0x03;
  const int m = (mdmcfg4 >> 4) & 0x03;
  return xtalHz / (8.0 * (4 + m) * static_cast<double>(1 << e));
}

uint8_t encodeChanBw(double hz, double xtalHz) {
  uint8_t best = 0;
  double bestErr = 1e300;
  for (int e = 0; e < 4; ++e) {
    for (int m = 0; m < 4; ++m) {
      const uint8_t field = static_cast<uint8_t>((e << 6) | (m << 4));
      const double err = std::fabs(chanBwHz(field, xtalHz) - hz);
      if (err < bestErr) {
        bestErr = err;
        best = field;
      }
    }
  }
  return best;
}

double deviationHz(uint8_t deviatn, double xtalHz) {
  const int e = (deviatn >> 4) & 0x07;
  const int m = deviatn & 0x07;
  return xtalHz / 131072.0 * (8 + m) * static_cast<double>(1 << e);
}

uint8_t encodeDeviation(double hz, double xtalHz) {
  uint8_t best = 0;
  double bestErr = 1e300;
  for (int e = 0; e < 8; ++e) {
    for (int m = 0; m < 8; ++m) {
      const uint8_t v = static_cast<uint8_t>((e << 4) | m);
      const double err = std::fabs(deviationHz(v, xtalHz) - hz);
      if (err < bestErr) {
        bestErr = err;
        best = v;
      }
    }
  }
  return best;
}

double focLimitHz(uint8_t foccfg, double chanBw) {
  switch (foccfg & 0x03) {
    case 0: return 0.0;
    case 1: return chanBw / 8.0;
    case 2: return chanBw / 4.0;
    default: return chanBw / 2.0;
  }
}

double wattsToDbm(double watts) { return 10.0 * std::log10(watts * 1000.0); }

double eirpDbmFromFieldUvPerM(double microvoltsPerMetre, double metres) {
  const double e = microvoltsPerMetre * 1e-6;
  const double w = (e * metres) * (e * metres) / 30.0;
  return wattsToDbm(w);
}

}  // namespace cc1101
}  // namespace openhsdu
