// One USB serial port, two jobs. The ESP32 Arduino framework supports only one USB serial port, so the console
// shares the data port, and the port's baud rate says which one it is:
//
//     115200 baud  ->  the text console (the bench tool opens it this way)
//     anything else ->  the HSDU data port (a host program opens it at 4800, like the real HSDU)
//
// A USB serial port does not carry the baud rate over the air, so the setting is free to mean this. In console
// mode the modem is told DTR and RTS are both low, so opening the port at 115200 can never key the transmitter,
// and nothing the modem receives is sent to the console. Back in data mode the real line state is restored.
#pragma once
#include <cstddef>
#include <cstdint>

#include "openhsdu/console.h"
#include "openhsdu/hal.h"
#include "openhsdu/modem.h"
#include "openhsdu/ring_buffer.h"

namespace openhsdu {

class HostMux : public IConsoleOut {
 public:
  static constexpr uint32_t kConsoleBaud = 115200;

  HostMux(Modem& modem, IHostLink& host) : modem_(modem), host_(host) {}
  void setConsole(Console* console) { console_ = console; }

  // From the USB serial port.
  void onLineCoding(uint32_t baud);                          // the PC set the baud rate
  void onLineState(bool dtr, bool rts);                      // the PC set DTR/RTS
  void onHostBytes(const uint8_t* data, size_t len);         // the PC sent bytes
  void poll();                                               // flush queued console text to the port

  bool consoleMode() const { return baud_ == kConsoleBaud; }
  uint32_t baud() const { return baud_; }

  // IConsoleOut: console text, queued and sent by poll(); dropped when not in console mode.
  void write(const char* text, size_t len) override;

 private:
  void applyLines();

  Modem& modem_;
  IHostLink& host_;
  Console* console_ = nullptr;
  uint32_t baud_ = 0;
  bool dtr_ = false;
  bool rts_ = false;
  RingBuffer<2048> out_;
};

}  // namespace openhsdu
