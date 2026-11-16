// Randomised event sequences against invariants. Deterministic: every sequence comes from a numbered seed, so
// a failure names the seed to replay.
//
// Invariants, for any order of PC and radio events:
//   I1  a byte only ever goes on the wire while the chip is transmitting;
//   I2  the carrier never stays up longer than the watchdog limit plus one tick;
//   I3  every byte the PC sent is either sent once, in order, or counted as dropped (nothing vanishes);
//   I4  the PC only ever receives echoes of what was sent (in order) and bytes the radio produced (in order);
//   I5  after the inputs stop, the modem returns to receive and the chip is receiving (liveness).
#include <algorithm>
#include <cstdint>
#include <random>

#include "bench.h"
#include "doctest.h"

using namespace openhsdu;
using sim::Bench;

namespace {

struct Run {
  uint64_t hostSent = 0;
  std::vector<uint8_t> radioInjected;  // bytes >= 0x80 only
};

// Is `sub` a subsequence of `seq`?
bool isSubsequence(const std::vector<uint8_t>& sub, const std::vector<uint8_t>& seq) {
  size_t j = 0;
  for (size_t i = 0; i < seq.size() && j < sub.size(); ++i)
    if (seq[i] == sub[j]) ++j;
  return j == sub.size();
}

void fuzzOne(uint32_t seed, KeyMode mode, bool withFaults) {
  std::mt19937 rng(seed);
  ModemConfig cfg;
  cfg.keyMode = mode;
  cfg.maxTxUs = 400000;  // short, so the watchdog is exercised too
  Bench b(cfg, static_cast<uint32_t>(rng()));  // random clock start, so wrap-around is hit regularly
  REQUIRE(b.boot());

  std::vector<uint8_t> sentByHost;
  std::vector<uint8_t> injected;
  bool dtr = true, rts = false;
  b.lines(dtr, rts);

  for (int step = 0; step < 1500; ++step) {
    switch (rng() % 12) {
      case 0: dtr = !dtr || (rng() % 4 != 0); b.lines(dtr, rts); break;
      case 1:
      case 2: rts = !rts; b.lines(dtr, rts); break;
      case 3:
      case 4: {  // bytes from the PC, 0x00..0x7F
        std::vector<uint8_t> v(rng() % 150);
        for (auto& x : v) x = static_cast<uint8_t>(rng() & 0x7F);
        b.pcWrite(v);
        sentByHost.insert(sentByHost.end(), v.begin(), v.end());
        break;
      }
      case 5: {  // bytes from the radio, 0x80..0xFF
        std::vector<uint8_t> v(rng() % 60);
        for (auto& x : v) x = static_cast<uint8_t>(0x80 | (rng() & 0x7F));
        b.radioRx(v);
        injected.insert(injected.end(), v.begin(), v.end());
        break;
      }
      case 6: b.host.blocked = (rng() % 3 == 0); break;
      case 7: b.host.maxPerWrite = (rng() % 2) ? 1 + rng() % 70 : static_cast<size_t>(-1); break;
      case 8:
        if (withFaults) b.chip.ignoreNextStrobes = static_cast<int>(rng() % 3);
        break;
      default: break;
    }
    b.run(50 + rng() % 6000, 50 + rng() % 400);
  }

  // The inputs stop: release everything, give time for every buffer to drain and every timer to expire.
  b.chip.ignoreNextStrobes = 0;
  b.host.blocked = false;
  b.host.maxPerWrite = static_cast<size_t>(-1);
  dtr = true;
  rts = false;
  b.lines(dtr, rts);
  b.run(4000000, 500);

  INFO("seed " << seed << " mode " << int(mode) << " faults " << withFaults);

  // I1 (a byte already in the UART's shift register when the carrier is cut by force finishes after it, so the
  // invariant is about bytes that START)
  CHECK(b.allBytesStartedInTx());

  // I2: no carrier window longer than the limit plus a tick (windows that end because of a lost strobe are
  // excluded when faults are injected, since the chip then really stays in TX until the retry)
  if (!withFaults) {
    uint32_t openAt = 0;
    bool open = false;
    for (const auto& c : b.chip.history) {
      if (c.marc == cc1101::kMarcTx && !open) {
        open = true;
        openAt = c.t;
      } else if (c.marc != cc1101::kMarcTx && open) {
        open = false;
        CHECK(c.t - openAt <= cfg.maxTxUs + 20000);
      }
    }
    CHECK_FALSE(open);
  }

  // I3: the bytes on the wire are exactly the PC's bytes with some removed, in order, and the counts add up
  const std::vector<uint8_t> onWire = b.uart.bytes();
  CHECK(isSubsequence(onWire, sentByHost));
  const Modem::Stats& s = b.modem->stats();
  INFO("hostIn=" << s.hostBytesIn << " tx=" << s.txBytes << " aborted=" << s.txAborted << " noKey=" << s.txDroppedNoKey
       << " full=" << s.txDroppedFull << " fault=" << s.txDroppedFault << " wire=" << onWire.size());
  CHECK(s.txBytes - s.txAborted == onWire.size());
  CHECK(s.hostBytesIn == sentByHost.size());
  // everything the PC wrote was handed to the UART (some of it later aborted) or counted as dropped
  CHECK(s.txBytes + s.txDroppedNoKey + s.txDroppedFull + s.txDroppedFault == s.hostBytesIn);

  // I4: what the PC received is the echo (a prefix-preserving subsequence of the wire bytes) interleaved with
  // radio bytes (a subsequence of what the radio produced)
  std::vector<uint8_t> echo, fromRadio;
  for (uint8_t v : b.host.received) (v & 0x80 ? fromRadio : echo).push_back(v);
  CHECK(isSubsequence(echo, sentByHost));
  CHECK(echo.size() <= s.txBytes);
  if (s.txAborted == 0) CHECK(isSubsequence(echo, onWire));
  CHECK(isSubsequence(fromRadio, injected));

  // I5
  CHECK_FALSE(b.modem->keyed());
  CHECK(b.chip.marc() == cc1101::kMarcRx);
  CHECK_FALSE(b.uart.busy());
}

}  // namespace

TEST_SUITE("fuzz") {

TEST_CASE("RTS mode, 120 seeds") {
  for (uint32_t seed = 1; seed <= 120; ++seed) fuzzOne(seed, KeyMode::kRts, false);
}

TEST_CASE("data mode, 120 seeds") {
  for (uint32_t seed = 1001; seed <= 1120; ++seed) fuzzOne(seed, KeyMode::kData, false);
}

TEST_CASE("either mode, 120 seeds") {
  for (uint32_t seed = 2001; seed <= 2120; ++seed) fuzzOne(seed, KeyMode::kEither, false);
}

TEST_CASE("either mode with lost strobes, 120 seeds") {
  for (uint32_t seed = 3001; seed <= 3120; ++seed) fuzzOne(seed, KeyMode::kEither, true);
}

}  // TEST_SUITE
