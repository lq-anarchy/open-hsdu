// A line-oriented text console for bring-up and bench work. It shares the one USB port with the data port through
// HostMux: the port is the console while the host has it at 115200 baud.
// It is protocol-blind like the rest: it can key a plain carrier and send a run of 0xAA (the preamble byte,
// a clean square wave for an SDR), nothing else. Type `help`.
#pragma once
#include <cstddef>
#include <cstdint>

#include "openhsdu/device.h"
#include "openhsdu/hal.h"
#include "openhsdu/modem.h"

namespace openhsdu {

class Console {
 public:
  Console(Device& device, Modem& modem, IConsoleOut& out) : dev_(device), modem_(modem), out_(out) {}

  // Characters from the console port. A line ends at CR or LF; empty lines are ignored.
  void feed(const uint8_t* data, size_t len);
  void feedString(const char* text);
  // Forget a half-typed line (the port closed or changed mode: the next session must not finish it).
  void reset() {
    len_ = 0;
    overflow_ = false;
  }

  static constexpr size_t kMaxLine = 80;
  static constexpr uint32_t kMaxCarrierMs = 1000;
  static constexpr size_t kMaxPatternBytes = 64;

 private:
  void execute(char* line);
  void print(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

  void cmdHelp();
  void cmdVersion();
  void cmdStatus();
  void cmdSelfTest();
  void cmdRssi();
  void cmdReg(char* arg);
  void cmdRegs();
  void cmdPower(char* arg);
  void cmdPowers();
  void cmdProfile(char* arg);
  void cmdKeyMode(char* arg);
  void cmdCarrier(char* arg);
  void cmdPattern(char* arg);

  Device& dev_;
  Modem& modem_;
  IConsoleOut& out_;
  char line_[kMaxLine + 1] = {};
  size_t len_ = 0;
  bool overflow_ = false;
};

}  // namespace openhsdu
