// The power table and the radiated-power arithmetic behind the defaults.
#include <ios>
#include <set>

#include "doctest.h"
#include "openhsdu/cc1101_math.h"
#include "openhsdu/power.h"

using namespace openhsdu;

TEST_SUITE("power") {

TEST_CASE("the table is sorted, has no duplicate bytes, and excludes PATABLE 0x00") {
  size_t n = 0;
  const PowerStep* s = powerSteps(&n);
  REQUIRE(n >= 8);
  std::set<uint8_t> seen;
  for (size_t i = 0; i < n; ++i) {
    CHECK(seen.insert(s[i].patable).second);
    CHECK(s[i].patable != 0x00);  // its harmonics are stronger than its carrier
    if (i > 0) CHECK(s[i].chipDbm >= s[i - 1].chipDbm);
  }
}

TEST_CASE("findPowerStep") {
  REQUIRE(findPowerStep(0x01) != nullptr);
  CHECK(findPowerStep(0x01)->chipDbm == doctest::Approx(-36.5));
  CHECK(findPowerStep(0xC0)->chipDbm == doctest::Approx(10.0));
  CHECK(findPowerStep(0x00) == nullptr);
  CHECK(findPowerStep(0xFF) == nullptr);
  CHECK(findPowerStep(0x77) == nullptr);
}

TEST_CASE("the default step is allowed and is the quietest one") {
  size_t n = 0;
  const PowerStep* s = powerSteps(&n);
  REQUIRE(findPowerStep(kDefaultPatable) != nullptr);
  CHECK(findPowerStep(kDefaultPatable)->chipDbm == s[0].chipDbm);
  CHECK(findPowerStep(kDefaultPatable)->chipDbm <= kDefaultMaxChipDbm);
}

TEST_CASE("by default exactly the three lowest steps are allowed") {
  size_t n = 0, allowed = 0;
  const PowerStep* s = powerSteps(&n);
  for (size_t i = 0; i < n; ++i)
    if (s[i].chipDbm <= kDefaultMaxChipDbm) ++allowed;
  CHECK(allowed == 3);
}

TEST_CASE("with a 20 dB attenuator and a 2 dBi antenna every allowed step is under the 15.209 limit") {
  size_t n = 0;
  const PowerStep* s = powerSteps(&n);
  const double limit = part15_209_limitEirpDbm();
  for (size_t i = 0; i < n; ++i) {
    if (s[i].chipDbm > kDefaultMaxChipDbm) continue;
    const EirpBudget b{s[i].chipDbm, 20.0, 2.0, 0.0};
    INFO("patable 0x" << std::hex << int(s[i].patable) << " eirp " << b.eirpDbm());
    CHECK(b.eirpDbm() < limit);
  }
}

TEST_CASE("margins: the loudest allowed step leaves 2.6 dB, the default leaves 5.3 dB") {
  const double limit = part15_209_limitEirpDbm();
  const EirpBudget loud{-33.8, 20.0, 2.0, 0.0};
  const EirpBudget quiet{findPowerStep(kDefaultPatable)->chipDbm, 20.0, 2.0, 0.0};
  CHECK(limit - loud.eirpDbm() == doctest::Approx(2.6).epsilon(0.03));
  CHECK(limit - quiet.eirpDbm() == doctest::Approx(5.3).epsilon(0.03));
}

TEST_CASE("the cap really is the line: the next step up would break the limit with the same attenuator") {
  const double limit = part15_209_limitEirpDbm();
  const EirpBudget next{findPowerStep(0x12)->chipDbm, 20.0, 2.0, 0.0};
  CHECK(next.eirpDbm() > limit);
}

TEST_CASE("without an attenuator no step is anywhere near legal, which is why the attenuator is not optional") {
  const double limit = part15_209_limitEirpDbm();
  const EirpBudget bare{findPowerStep(kDefaultPatable)->chipDbm, 0.0, 2.0, 0.0};
  CHECK(bare.eirpDbm() > limit + 10.0);
  const EirpBudget flipperLike{-20.0, 0.0, 0.0, 0.0};  // a typical hobby transmitter preset at its maximum
  CHECK(flipperLike.eirpDbm() - limit > 28.0);
}

TEST_CASE("EirpBudget adds gains and subtracts losses") {
  const EirpBudget b{-10.0, 20.0, 3.0, 1.0};
  CHECK(b.eirpDbm() == doctest::Approx(-28.0));
}

}  // TEST_SUITE
