#include <set>

#include "../firmware/src/pins.h"
#include "doctest.h"

TEST_SUITE("wiring") {

TEST_CASE("no signal is on a boot-strap, USB or LED pin") {
  for (int p : pins::kAll)
    for (int bad : pins::kForbidden) CHECK(p != bad);
}

TEST_CASE("no two signals share a pin") {
  std::set<int> seen;
  for (int p : pins::kAll) CHECK(seen.insert(p).second);
}

TEST_CASE("every pin is a real ESP32-S3 GPIO") {
  for (int p : pins::kAll) {
    CHECK(p >= 0);
    CHECK(p <= 48);
    CHECK_FALSE((p >= 22 && p <= 25));  // not present on the S3
    CHECK_FALSE((p >= 26 && p <= 32));  // flash/PSRAM
  }
}

}  // TEST_SUITE
