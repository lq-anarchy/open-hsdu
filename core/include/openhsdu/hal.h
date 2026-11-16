// The only things the core needs from the outside world. The firmware implements these on the ESP32-S3;
// the tests implement them with a fake clock, a fake UART and a model of the CC1101.
#pragma once
#include <cstddef>
#include <cstdint>

namespace openhsdu {

// SPI to the CC1101: chip select and one byte at a time (the chip wants CSN held low for a whole transaction).
class ISpi {
 public:
  virtual ~ISpi() = default;
  virtual void select(bool active) = 0;
  virtual uint8_t transfer(uint8_t out) = 0;
  virtual bool misoLow() = 0;  // the chip pulls MISO low when its crystal is running and it is ready for SPI
};

// A free-running 32-bit microsecond counter (wraps every 71.6 minutes) and a short busy delay.
class IClock {
 public:
  virtual ~IClock() = default;
  virtual uint32_t micros() = 0;
  virtual void delayUs(uint32_t us) = 0;
};

// What the modem needs from the radio chip.
class IRadio {
 public:
  virtual ~IRadio() = default;
  virtual bool enterTx() = 0;  // returns true once the chip is transmitting with its PLL locked
  virtual bool enterRx() = 0;  // returns true once the chip is receiving
  virtual bool transmitting() = 0;  // does the chip itself say it is in transmit? (a read, not a belief)
};

// The hardware UART wired to the chip's two serial pins: 4800 baud, 8N1.
class IUart {
 public:
  virtual ~IUart() = default;
  virtual size_t write(const uint8_t* data, size_t len) = 0;  // returns how many bytes were accepted
  virtual size_t writable() = 0;                              // free space in the transmit queue, in bytes
  virtual bool busy() = 0;                                    // true while any byte is still being clocked out
  virtual void flushRx() = 0;                                 // drop anything received so far
  // Throw away every byte still queued in the transmit FIFO and return how many. A byte already in the shift
  // register finishes. Used when the carrier is cut by force, so stale bytes cannot leak into the next
  // transmission.
  virtual size_t abortTx() = 0;
};

// The USB serial port the PC sees. Accepts as many bytes as fit and returns the count.
class IHostLink {
 public:
  virtual ~IHostLink() = default;
  virtual size_t write(const uint8_t* data, size_t len) = 0;
};

// Text out of the console (HostMux queues it for the one USB port).
class IConsoleOut {
 public:
  virtual ~IConsoleOut() = default;
  virtual void write(const char* text, size_t len) = 0;
};

}  // namespace openhsdu
