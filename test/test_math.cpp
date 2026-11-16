// The CC1101's equations against the numbers that were measured on the real link. Each test is a line of
// docs/THEORY.md, "The chip's equations".
#include <cmath>

#include "doctest.h"
#include "openhsdu/cc1101_math.h"
#include "openhsdu/power.h"

using namespace openhsdu::cc1101;

TEST_SUITE("math") {

TEST_CASE("carrier frequency word: 0x1013B1 is 418.000 MHz to within 200 Hz") {
  const uint32_t w = freqWord(0x10, 0x13, 0xB1);
  CHECK(w == 1053617u);
  CHECK(std::fabs(carrierHz(w) - 418.000e6) < 200.0);
}

TEST_CASE("carrier frequency word: 0x1013CF is the HSDU's 418.012 MHz to within 500 Hz") {
  const uint32_t w = freqWord(0x10, 0x13, 0xCF);
  CHECK(w == 1053647u);
  CHECK(std::fabs(carrierHz(w) - 418.012e6) < 500.0);
}

TEST_CASE("freqWordForHz inverts carrierHz to the nearest word") {
  CHECK(freqWordForHz(418.000e6) == freqWord(0x10, 0x13, 0xB1));
  CHECK(freqWordForHz(418.012e6) == freqWord(0x10, 0x13, 0xCF));
  for (uint32_t w : {1000000u, 1053617u, 1100000u}) CHECK(freqWordForHz(carrierHz(w)) == w);
}

TEST_CASE("FREQ2 only has six usable bits") {
  CHECK(freqWord(0xFF, 0x00, 0x00) == freqWord(0x3F, 0x00, 0x00));
}

TEST_CASE("data rate: MDMCFG4 0x67 / MDMCFG3 0x83 is 4,797.9 baud, 0.04% from the link's 4,800") {
  const double r = dataRateBaud(0x67, 0x83);
  CHECK(std::fabs(r - 4797.935) < 0.01);
  CHECK(std::fabs(r - 4800.0) / 4800.0 < 0.0005);
}

TEST_CASE("data rate: only the low nibble of MDMCFG4 is the exponent") {
  CHECK(dataRateBaud(0x67, 0x83) == dataRateBaud(0xF7, 0x83));
}

TEST_CASE("encodeDataRate(4800) finds exponent 7, mantissa 0x83") {
  const DataRateRegs r = encodeDataRate(4800.0);
  CHECK(r.ok);
  CHECK(r.exponent == 7);
  CHECK(r.mantissa == 0x83);
  CHECK(std::fabs(r.actualBaud - 4800.0) < 2.5);
}

TEST_CASE("encodeDataRate round-trips across the datasheet's range") {
  for (double baud : {600.0, 1200.0, 2400.0, 9600.0, 38400.0, 115200.0, 250000.0}) {
    const DataRateRegs r = encodeDataRate(baud);
    REQUIRE(r.ok);
    CHECK(std::fabs(r.actualBaud - baud) / baud < 0.005);
  }
}

TEST_CASE("encodeDataRate refuses what the chip cannot do") {
  CHECK_FALSE(encodeDataRate(0.0).ok);
  CHECK_FALSE(encodeDataRate(1.0e9).ok);
  CHECK_FALSE(encodeDataRate(-5.0).ok);
}

TEST_CASE("channel filter: MDMCFG4 0x67 is 270.8 kHz") {
  CHECK(std::fabs(chanBwHz(0x67) - 270833.0) < 1.0);
}

TEST_CASE("channel filter: the datasheet's extremes") {
  CHECK(std::fabs(chanBwHz(0x00) - 812500.0) < 1.0);  // E=0, M=0: 26 MHz / 32
  CHECK(std::fabs(chanBwHz(0xF0) - 58035.7) < 1.0);   // E=3, M=3: 26 MHz / 448
}

TEST_CASE("encodeChanBw picks the nearest filter and leaves the data-rate nibble clear") {
  CHECK(encodeChanBw(270.8e3) == 0x60);
  CHECK((encodeChanBw(100e3) & 0x0F) == 0);
  CHECK(std::fabs(chanBwHz(encodeChanBw(100e3)) - 100e3) <= 8e3);
}

TEST_CASE("deviation: DEVIATN 0x47 is 47.6 kHz") {
  CHECK(std::fabs(deviationHz(0x47) - 47607.0) < 5.0);
}

TEST_CASE("encodeDeviation: nearest step, and 0x45 is the step nearest the HSDU's measured 42.5 kHz") {
  CHECK(encodeDeviation(47.6e3) == 0x47);
  CHECK(encodeDeviation(42.5e3) == 0x45);
  CHECK(std::fabs(deviationHz(0x45) - 41259.0) < 5.0);
  CHECK(std::fabs(deviationHz(0x46) - 44432.0) < 5.0);
}

TEST_CASE("deviation covers 1.6 to 380 kHz") {
  CHECK(std::fabs(deviationHz(0x00) - 1587.0) < 5.0);
  CHECK(std::fabs(deviationHz(0x77) - 380859.0) < 5.0);
}

TEST_CASE("frequency-offset compensation limit: FOCCFG 0x16 allows about +/-68 kHz of a pack's drift") {
  const double bw = chanBwHz(0x67);
  CHECK(std::fabs(focLimitHz(0x16, bw) - 67708.0) < 5.0);
  CHECK(focLimitHz(0x14, bw) == 0.0);
  CHECK(focLimitHz(0x15, bw) == doctest::Approx(bw / 8.0));
  CHECK(focLimitHz(0x17, bw) == doctest::Approx(bw / 2.0));
}

TEST_CASE("the packs' worst carrier offset fits inside the compensation limit") {
  // Data links measured 417.979 to 418.020 MHz against a 418.000 MHz receiver: at most 21 kHz off.
  const double offsets[] = {418.020e6 - 418.000e6, 417.979e6 - 418.000e6, 417.994e6 - 418.000e6};
  for (double o : offsets) CHECK(std::fabs(o) < focLimitHz(0x16, chanBwHz(0x67)));
}

TEST_CASE("field strength to EIRP: 200 uV/m at 3 m is -49.2 dBm") {
  CHECK(eirpDbmFromFieldUvPerM(200.0, 3.0) == doctest::Approx(-49.208).epsilon(0.001));
  CHECK(openhsdu::part15_209_limitEirpDbm() == doctest::Approx(-49.208).epsilon(0.001));
}

TEST_CASE("EIRP follows the inverse-square law in distance and 6 dB per doubling of field") {
  CHECK(eirpDbmFromFieldUvPerM(400.0, 3.0) - eirpDbmFromFieldUvPerM(200.0, 3.0) == doctest::Approx(6.0206).epsilon(0.001));
  CHECK(eirpDbmFromFieldUvPerM(200.0, 6.0) - eirpDbmFromFieldUvPerM(200.0, 3.0) == doctest::Approx(6.0206).epsilon(0.001));
}

TEST_CASE("wattsToDbm") {
  CHECK(wattsToDbm(0.001) == doctest::Approx(0.0).epsilon(1e-9));
  CHECK(wattsToDbm(1.0) == doctest::Approx(30.0));
}

TEST_CASE("elapsedUs is wrap-safe and never negative") {
  CHECK(elapsedUs(100, 40) == 60u);
  CHECK(elapsedUs(5, 0xFFFFFFF0u) == 21u);   // the counter wrapped between the two readings
  CHECK(elapsedUs(40, 100) == 0u);           // `since` is in the future: report no time, not 4 billion
  CHECK(elapsedUs(7, 7) == 0u);
}

}  // TEST_SUITE
