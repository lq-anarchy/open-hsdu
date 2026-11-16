// The PC end of the USB serial port, as the modem sees it: it collects the bytes sent to the PC and can be
// told to accept only part of each write (a full USB buffer) or nothing at all.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "openhsdu/hal.h"

namespace sim {

class FakeHost : public openhsdu::IHostLink {
 public:
  size_t write(const uint8_t* data, size_t len) override {
    size_t n = len < maxPerWrite ? len : maxPerWrite;
    if (blocked) n = 0;
    received.insert(received.end(), data, data + n);
    return n;
  }

  std::vector<uint8_t> received;
  size_t maxPerWrite = static_cast<size_t>(-1);
  bool blocked = false;
};

// Console output collector.
class FakeConsoleOut : public openhsdu::IConsoleOut {
 public:
  void write(const char* text, size_t len) override { text_.append(text, len); }
  const std::string& text() const { return text_; }
  void clear() { text_.clear(); }
  bool has(const char* needle) const { return text_.find(needle) != std::string::npos; }

 private:
  std::string text_;
};

}  // namespace sim
