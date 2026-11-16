#include "openhsdu/modem.h"

#include <algorithm>

#include "openhsdu/cc1101_math.h"

namespace openhsdu {

using cc1101::elapsedUs;

Modem::Modem(const ModemConfig& cfg, IRadio& radio, IUart& uart, IHostLink& host, IClock& clock)
    : cfg_(cfg), radio_(radio), uart_(uart), host_(host), clock_(clock) {
  rxSince_ = clock_.micros();
  lastRxCheckAt_ = rxSince_;
}

void Modem::setRadioReady(bool ready) { radioReady_ = ready; }

void Modem::holdRts() {
  rtsHold_ = true;
  rtsHoldAt_ = clock_.micros();
}

void Modem::onLineState(bool dtr, bool rts) {
  if (dtr_ && !dtr) hostOut_.clear();  // the PC closed the port: nothing queued for it is wanted any more
  const bool rising = rts && !rts_;
  dtr_ = dtr;
  rts_ = rts;
  if (!rts) lockout_ = false;          // the lockout lasts until the PC has let go once
  // A fresh RTS request after a watchdog trip is a deliberate act and is honoured; only a stream of bytes with no
  // RTS (or a host stuck with RTS held) stays locked out. In key mode data RTS means nothing, so it clears nothing.
  if (rising && !lockout_ && cfg_.keyMode != KeyMode::kData) dataLockout_ = false;
}

void Modem::onHostBytes(const uint8_t* data, size_t len) {
  stats_.hostBytesIn += len;
  lastHostByteAt_ = clock_.micros();
  // Bytes the radio cannot send now are dropped and counted, not queued to go out minutes later when it recovers.
  if (!radioReady_ || dataLockout_ || needRxRecovery_ || faultBackoff_) {
    stats_.txDroppedFault += len;
    return;
  }
  if (state_ == State::kRx && cfg_.keyMode == KeyMode::kRts && !rtsHolds()) {
    stats_.txDroppedNoKey += len;
    return;
  }
  const size_t stored = txBuf_.push(data, len);
  stats_.txDroppedFull += len - stored;
}

void Modem::onUartRx(const uint8_t* data, size_t len) {
  if (state_ != State::kRx || needRxRecovery_) {
    stats_.rxDiscardedTx += len;
    return;
  }
  if (settling_ && elapsedUs(clock_.micros(), rxSince_) < cfg_.rxSettleUs) {
    stats_.rxDiscardedSettle += len;
    return;
  }
  stats_.rxBytes += len;
  pushHost(data, len);
}

bool Modem::testActive() const {
  return testActive_ && static_cast<int32_t>(testUntil_ - clock_.micros()) > 0;
}

bool Modem::carrierBurst(uint32_t durationUs) {
  if (!radioReady_ || state_ != State::kRx || needRxRecovery_ || faultBackoff_ || durationUs == 0) return false;
  testActive_ = true;
  // Capped at half the watchdog so a test carrier can never be what trips it.
  testUntil_ = clock_.micros() + std::min(durationUs, cfg_.maxTxUs / 2);
  return true;
}

bool Modem::sendBytes(const uint8_t* data, size_t len) {
  if (!radioReady_ || state_ != State::kRx || needRxRecovery_ || faultBackoff_ || len == 0) return false;
  forceData_ = true;
  const size_t stored = txBuf_.push(data, len);
  stats_.txDroppedFull += len - stored;
  return stored == len;
}

bool Modem::wantKey() const {
  if (!radioReady_ || faultBackoff_) return false;
  if (rtsHolds()) return true;
  if (!txBuf_.empty() && (cfg_.keyMode != KeyMode::kRts || forceData_)) return true;
  return testActive();
}

void Modem::startTx() {
  keyedByRts_ = rtsHolds();
  if (!radio_.enterTx()) {
    ++stats_.radioFaults;
    ++stats_.keyUpFailures;
    // The chip may be half way to TX: ask for receive, and keep asking (paced) if it will not go.
    if (!radio_.enterRx()) {
      ++stats_.radioFaults;
      needRxRecovery_ = true;
      lastRxCheckAt_ = clock_.micros() - cfg_.rxCheckUs;  // retry on the next tick
    }
    faultBackoff_ = true;
    faultAt_ = clock_.micros();
    stats_.txDroppedFault += txBuf_.size();
    txBuf_.clear();
    forceData_ = false;
    testActive_ = false;
    return;
  }
  faultBackoff_ = false;
  ++stats_.keyUps;
  keyedAt_ = clock_.micros();  // after the chip reports TX: the PLL is locked from here
  leadUs_ = keyedByRts_ ? cfg_.radioSettleUs : std::max(cfg_.radioSettleUs, cfg_.dataKeyLeadUs);
  state_ = State::kTxLead;
}

void Modem::stopTx() {
  if (!radio_.enterRx()) {
    ++stats_.radioFaults;
    needRxRecovery_ = true;
    lastRxCheckAt_ = clock_.micros() - cfg_.rxCheckUs;  // retry on the next tick
  } else {
    needRxRecovery_ = false;
  }
  uart_.flushRx();
  state_ = State::kRx;
  rxSince_ = clock_.micros();
  settling_ = true;
  keyedByRts_ = false;
  forceData_ = false;
  testActive_ = false;
}

void Modem::forceRx(bool dropTxBytes) {
  if (dropTxBytes) {
    stats_.txDroppedFault += txBuf_.size();
    txBuf_.clear();
    stats_.txAborted += static_cast<uint32_t>(uart_.abortTx());
  }
  stopTx();
}

bool Modem::idleTx() const {
  return txBuf_.empty() && !uart_.busy() && !rtsHolds() && !testActive();
}

void Modem::pushHost(const uint8_t* data, size_t len) {
  if (!dtr_) {
    stats_.hostDroppedNoDtr += len;
    return;
  }
  const size_t stored = hostOut_.push(data, len);
  stats_.hostDroppedFull += len - stored;
}

void Modem::flushHost() {
  uint8_t tmp[64];
  while (dtr_ && !hostOut_.empty()) {
    const size_t n = hostOut_.peek(tmp, sizeof(tmp));
    const size_t w = host_.write(tmp, n);
    hostOut_.drop(w);
    if (w < n) break;  // the USB side is full; try again next tick
  }
}

void Modem::pump() {
  uint8_t tmp[64];
  size_t room = uart_.writable();
  while (room > 0 && !txBuf_.empty()) {
    const size_t want = std::min({room, sizeof(tmp), txBuf_.size()});
    const size_t n = txBuf_.peek(tmp, want);
    const size_t w = uart_.write(tmp, n);
    txBuf_.drop(w);
    stats_.txBytes += w;
    room -= w;
    if (cfg_.echo && w > 0) pushHost(tmp, w);
    if (w < n) break;
  }
}

// Every timer that is only meaningful for a while is closed here, by a flag, as soon as its time is up. A flag set
// by an earlier event and read much later through a 32-bit time comparison would start to lie after 35.8 minutes
// (the comparison is signed), which is how a receiver can go deaf or a carrier can re-arm itself.
void Modem::housekeeping() {
  const uint32_t now = clock_.micros();
  if (settling_ && elapsedUs(now, rxSince_) >= cfg_.rxSettleUs) settling_ = false;
  if (rtsHold_ && elapsedUs(now, rtsHoldAt_) >= cfg_.rtsHoldoffUs) rtsHold_ = false;
  // The one stored timestamp that is compared on every receive tick: if the loop ever stalled for 2^31 us it would
  // read as 'in the future' and the 20 ms chip check would stay off for another 35 minutes. Re-arm it instead.
  if (static_cast<int32_t>(now - lastRxCheckAt_) < 0) lastRxCheckAt_ = now - cfg_.rxCheckUs;
  if (faultBackoff_ && elapsedUs(now, faultAt_) >= cfg_.faultBackoffUs) faultBackoff_ = false;
  if (testActive_ && !testActive()) testActive_ = false;
  if (dataLockout_ && elapsedUs(now, lastHostByteAt_) >= cfg_.tripQuietUs) dataLockout_ = false;
}

void Modem::tick() {
  housekeeping();

  if (state_ == State::kRx) {
    // Bytes accepted while RTS was up, but RTS dropped before the radio could be keyed: the PC withdrew the
    // transmit request, so they must not linger and leak into the next transmission.
    if (!txBuf_.empty() && cfg_.keyMode == KeyMode::kRts && !rtsHolds() && !forceData_) {
      stats_.txDroppedNoKey += static_cast<uint32_t>(txBuf_.size());
      txBuf_.clear();
    }
    if (needRxRecovery_) {
      // The chip would not go back to receive: keep asking, paced so a dead chip is not hammered.
      if (elapsedUs(clock_.micros(), lastRxCheckAt_) >= cfg_.rxCheckUs) {
        lastRxCheckAt_ = clock_.micros();
        if (radio_.enterRx()) {
          needRxRecovery_ = false;
          rxSince_ = clock_.micros();
          settling_ = true;
        } else {
          ++stats_.radioFaults;
        }
      }
    } else if (wantKey()) {
      startTx();
    } else if (elapsedUs(clock_.micros(), lastRxCheckAt_) >= cfg_.rxCheckUs) {
      // The chip's own report, not the modem's belief: if it is transmitting when we think it is not (a lost
      // strobe, a glitch), say so and make it listen.
      lastRxCheckAt_ = clock_.micros();
      if (radioReady_ && radio_.transmitting()) {
        ++stats_.unexpectedTx;
        if (!radio_.enterRx()) {
          needRxRecovery_ = true;
          lastRxCheckAt_ = clock_.micros() - cfg_.rxCheckUs;
        }
      }
    }
  }

  if (state_ != State::kRx && elapsedUs(clock_.micros(), keyedAt_) >= cfg_.maxTxUs) {
    ++stats_.watchdogTrips;
    lockout_ = rts_;
    dataLockout_ = true;
    lastHostByteAt_ = clock_.micros();
    forceRx(true);
  }

  if (state_ == State::kTxLead && elapsedUs(clock_.micros(), keyedAt_) >= leadUs_) state_ = State::kTxData;

  if (state_ == State::kTxData) {
    pump();
    if (idleTx()) {
      tailUs_ = keyedByRts_ ? cfg_.txLatencyHoldUs : std::max(cfg_.dataKeyTailUs, cfg_.txLatencyHoldUs);
      tailStart_ = clock_.micros();
      state_ = State::kTxTail;
    }
  }

  if (state_ == State::kTxTail) {
    if (!txBuf_.empty() || rtsHolds() || testActive()) {
      state_ = State::kTxData;
      pump();
    } else if (elapsedUs(clock_.micros(), tailStart_) >= tailUs_) {
      stopTx();
    }
  }

  flushHost();
}

}  // namespace openhsdu
