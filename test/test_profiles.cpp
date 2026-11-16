// The register profiles: their values against the proven preset, and what each register decodes to.
#include <cmath>
#include <map>
#include <set>

#include "doctest.h"
#include "openhsdu/cc1101_math.h"
#include "openhsdu/cc1101_regs.h"
#include "openhsdu/profiles.h"

using namespace openhsdu;
using namespace openhsdu::cc1101;

namespace {
std::map<uint8_t, uint8_t> asMap(const Profile& p) {
  std::map<uint8_t, uint8_t> m;
  for (size_t i = 0; i < p.count; ++i) m[p.regs[i].addr] = p.regs[i].value;
  return m;
}
}  // namespace

TEST_SUITE("profiles") {

TEST_CASE("every profile register is a configuration register, written once") {
  for (const Profile* p : {&profileProven(), &profileHsduMatched()}) {
    std::set<uint8_t> seen;
    for (size_t i = 0; i < p->count; ++i) {
      INFO(p->name << " addr 0x" << std::hex << int(p->regs[i].addr));
      CHECK(p->regs[i].addr < kNumConfigRegs);
      CHECK(seen.insert(p->regs[i].addr).second);
    }
  }
}

TEST_CASE("the proven profile has the values of the preset that worked on real packs") {
  // The register values of the Flipper Zero preset that real packs acknowledged (docs/THEORY.md sections 3.2, 3.3, 9).
  const std::map<uint8_t, uint8_t> proven = {
      {FSCTRL1, 0x06}, {PKTCTRL0, 0x32}, {PKTCTRL1, 0x04}, {MDMCFG4, 0x67}, {MDMCFG3, 0x83}, {MDMCFG2, 0x04},
      {MDMCFG1, 0x02}, {MDMCFG0, 0x00},  {DEVIATN, 0x47},  {MCSM0, 0x18},   {FOCCFG, 0x16},  {AGCCTRL2, 0x07},
      {AGCCTRL1, 0x00}, {AGCCTRL0, 0x91}, {WORCTRL, 0xFB}, {FREND0, 0x10},  {FREND1, 0x56}};
  const Profile& p = profileProven();
  const auto m = asMap(p);
  for (const auto& kv : proven) {
    INFO("addr 0x" << std::hex << int(kv.first));
    REQUIRE(m.count(kv.first) == 1);
    CHECK(m.at(kv.first) == kv.second);
  }
  // ... and every register tagged "proven" is in that list: nothing was added under the proven label.
  for (size_t i = 0; i < p.count; ++i) {
    if (p.regs[i].source == RegSource::kProven) CHECK(proven.count(p.regs[i].addr) == 1);
  }
}

TEST_CASE("the five changes for a bare module are present and tagged") {
  const auto m = asMap(profileProven());
  CHECK(m.at(FREQ2) == 0x10);
  CHECK(m.at(FREQ1) == 0x13);
  CHECK(m.at(FREQ0) == 0xB1);
  CHECK(m.at(IOCFG0) == kIocfgHighZ);
  CHECK(m.at(IOCFG2) == kIocfgSerialDataOut);
  CHECK((m.at(MCSM1) & kCcaModeMask) == 0);
  for (size_t i = 0; i < profileProven().count; ++i) {
    const RegPair& r = profileProven().regs[i];
    if (r.addr == FREQ2 || r.addr == FREQ1 || r.addr == FREQ0 || r.addr == IOCFG0 || r.addr == IOCFG2 ||
        r.addr == MCSM1)
      CHECK(r.source == RegSource::kBareModule);
  }
}

TEST_CASE("the SmartRF extras are the only registers outside the proven list") {
  std::set<uint8_t> extras;
  for (size_t i = 0; i < profileProven().count; ++i)
    if (profileProven().regs[i].source == RegSource::kSmartRf) extras.insert(profileProven().regs[i].addr);
  const std::set<uint8_t> expected = {FSCAL3, FSCAL2, FSCAL1, FSCAL0, TEST2, TEST1, TEST0};
  CHECK(extras == expected);
}

TEST_CASE("registers decode to the profile's stated intent") {
  for (const Profile* p : {&profileProven(), &profileHsduMatched()}) {
    INFO(p->name);
    const auto m = asMap(*p);
    const double carrier = carrierHz(freqWord(m.at(FREQ2), m.at(FREQ1), m.at(FREQ0)));
    CHECK(std::fabs(carrier - p->carrierHz) < 500.0);
    CHECK(std::fabs(dataRateBaud(m.at(MDMCFG4), m.at(MDMCFG3)) - p->baud) / p->baud < 0.001);
    CHECK(std::fabs(deviationHz(m.at(DEVIATN)) - p->deviationHz) < 500.0);
    CHECK(std::fabs(chanBwHz(m.at(MDMCFG4)) - p->chanBwHz) < 1000.0);
  }
}

TEST_CASE("the configuration is asynchronous serial, 2-FSK, with the pins set for a UART") {
  for (const Profile* p : {&profileProven(), &profileHsduMatched()}) {
    INFO(p->name);
    const auto m = asMap(*p);
    CHECK((m.at(PKTCTRL0) & kPktFormatMask) == kPktFormatAsyncSerial);
    CHECK((m.at(MDMCFG2) & kModFormatMask) == kModFormat2Fsk);
    CHECK(m.at(IOCFG0) == kIocfgHighZ);            // the UART's transmit pin drives GDO0, the chip must not
    CHECK(m.at(IOCFG2) == kIocfgSerialDataOut);    // demodulated data comes out of GDO2
    CHECK((m.at(MCSM1) & kCcaModeMask) == 0);      // a pack's carrier must not block a transmit
    CHECK((m.at(MCSM0) & 0x30) == 0x10);           // calibrate on idle -> RX/TX
    CHECK(focLimitHz(m.at(FOCCFG), chanBwHz(m.at(MDMCFG4))) > 60e3);  // offset compensation is on
  }
}

TEST_CASE("the filter is wide enough for the widest sender: the packs' data links reach +/-51 kHz") {
  const auto m = asMap(profileProven());
  // A 2-FSK signal of deviation d and 4.8 kbaud needs about 2*(d + baud/2) of filter.
  const double widest = 2.0 * (51e3 + 2400.0);
  CHECK(chanBwHz(m.at(MDMCFG4)) > widest * 0.9);  // 270.8 kHz against 107 kHz needed, plus 40 kHz of offset
}

TEST_CASE("the matched profile differs from the proven one in exactly the carrier and the deviation") {
  const auto a = asMap(profileProven());
  const auto b = asMap(profileHsduMatched());
  REQUIRE(a.size() == b.size());
  std::set<uint8_t> diff;
  for (const auto& kv : a) {
    REQUIRE(b.count(kv.first) == 1);
    if (b.at(kv.first) != kv.second) diff.insert(kv.first);
  }
  const std::set<uint8_t> expected = {FREQ0, DEVIATN};
  CHECK(diff == expected);
}

TEST_CASE("the matched profile reproduces what was measured on the HSDU's own signal") {
  const auto m = asMap(profileHsduMatched());
  const double carrier = carrierHz(freqWord(m.at(FREQ2), m.at(FREQ1), m.at(FREQ0)));
  CHECK(std::fabs(carrier - 418.012e6) < 1000.0);                 // measured centre 418.011-418.012 MHz
  CHECK(std::fabs(deviationHz(m.at(DEVIATN)) - 42.5e3) < 2000.0);  // measured +/-42.5 to +/-43 kHz
  // Mark (upper tone) = carrier + deviation lands on the measured 418.054-418.055 MHz.
  CHECK(std::fabs(carrier + deviationHz(m.at(DEVIATN)) - 418.054e6) < 2500.0);
}

TEST_CASE("the proven profile's tones, from the numbers that were proven on the packs") {
  const auto m = asMap(profileProven());
  const double carrier = carrierHz(freqWord(m.at(FREQ2), m.at(FREQ1), m.at(FREQ0)));
  const double dev = deviationHz(m.at(DEVIATN));
  CHECK(std::fabs((carrier + dev) - 418.0476e6) < 500.0);  // mark
  CHECK(std::fabs((carrier - dev) - 417.9524e6) < 500.0);  // space
}

TEST_CASE("profileByName") {
  CHECK(profileByName("proven") == &profileProven());
  CHECK(profileByName("matched") == &profileHsduMatched());
  CHECK(profileByName("nope") == nullptr);
  CHECK(profileByName("") == nullptr);
  CHECK(profileByName(nullptr) == nullptr);
}

}  // TEST_SUITE
