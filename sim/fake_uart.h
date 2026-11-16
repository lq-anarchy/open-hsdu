// A UART that clocks bytes out at the real baud rate on the fake clock, with a 128-byte transmit FIFO like the
// ESP32's. It records when every byte started and finished on the (virtual) wire, so tests can ask whether the
// radio was transmitting at that moment.
#pragma once
#include <cstdint>
#include <vector>

#include "fake_clock.h"
#include "openhsdu/hal.h"

namespace sim {

class FakeUart : public openhsdu::IUart {
 public:
  struct Sent {
    uint8_t value;
    uint32_t startUs;
    uint32_t endUs;
  };

  explicit FakeUart(FakeClock& clock, uint32_t baud = 4800, size_t fifo = 128)
      : clock_(clock), byteUs_((10u * 1000000u + baud / 2) / baud), fifo_(fifo) {}

  size_t write(const uint8_t* data, size_t len) override {
    const size_t room = writable();
    const size_t n = len < room ? len : room;
    for (size_t i = 0; i < n; ++i) {
      uint32_t start = clock_.micros();
      if (!sent.empty() && static_cast<int32_t>(sent.back().endUs - start) > 0) start = sent.back().endUs;
      sent.push_back({data[i], start, start + byteUs_});
    }
    return n;
  }

  size_t writable() override {
    const size_t inflight = inFlight();
    return inflight >= fifo_ ? 0 : fifo_ - inflight;
  }

  bool busy() override { return inFlight() > 0; }
  void flushRx() override { ++flushRxCalls; }

  size_t abortTx() override {
    // Bytes that have not started are gone. A byte already in the shift register finishes (as on the ESP32).
    size_t n = 0;
    const uint32_t now = clock_.micros();
    while (!sent.empty() && static_cast<int32_t>(sent.back().startUs - now) > 0) {
      sent.pop_back();
      ++n;
    }
    return n;
  }

  size_t inFlight() const {
    size_t n = 0;
    const uint32_t now = clock_.micros();
    for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
      if (static_cast<int32_t>(it->endUs - now) > 0) ++n;
      else break;
    }
    return n;
  }

  std::vector<uint8_t> bytes() const {
    std::vector<uint8_t> v;
    for (const Sent& s : sent) v.push_back(s.value);
    return v;
  }

  uint32_t byteUs() const { return byteUs_; }

  std::vector<Sent> sent;
  int flushRxCalls = 0;

 private:
  FakeClock& clock_;
  uint32_t byteUs_;
  size_t fifo_;
};

}  // namespace sim
