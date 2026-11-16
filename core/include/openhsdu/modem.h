// The modem: a protocol-blind bridge between the PC's USB serial port and the radio. It never looks inside the
// bytes. This is the HSDU's whole job:
//
//   PC -> radio : while the PC holds RTS (or, if configured, when bytes arrive), key the transmitter, send the
//                 bytes at 4800 baud through the UART into the chip's data pin, echo them back to the PC the way
//                 the HSDU does, and return to receive when the PC lets go (or the bytes run out).
//   radio -> PC : in receive, forward every byte the chip's demodulator produces (no squelch, like the HSDU).
//
// Safety, all tested: the radio must be declared ready before it can ever be keyed; a watchdog drops the carrier
// if it has been up too long and then holds off data keying until the stream has been quiet; a carrier is never
// released before the chip has had time to send the last bit; a chip that reports TX while the modem thinks it is
// receiving is told to receive; a port that closes drops RTS.
// All time comes from IClock, so every behaviour is testable on a PC with a fake clock, and every timer that can
// outlive its purpose is cleared by a flag rather than left to a 32-bit comparison (which wraps after 35.8 min).
#pragma once
#include <cstddef>
#include <cstdint>

#include "openhsdu/hal.h"
#include "openhsdu/ring_buffer.h"

namespace openhsdu {

enum class KeyMode : uint8_t {
  kRts,     // transmit only while the PC holds RTS (what a host program does with the real HSDU)
  kData,    // ignore RTS; transmit when bytes arrive, with a lead-in and a tail added by the firmware
  kEither   // RTS or data, whichever comes first
};

struct ModemConfig {
  KeyMode keyMode = KeyMode::kEither;
  uint32_t radioSettleUs = 1000;      // after the chip reports TX, wait this long before the first byte
  uint32_t dataKeyLeadUs = 85000;     // carrier before the first byte when keyed by data (the HSDU's measured lead)
  uint32_t dataKeyTailUs = 80000;     // carrier after the last byte when keyed by data (the HSDU's measured tail)
  // Minimum carrier after the UART has sent the last bit, in every key mode. The chip's modulator has a latency of
  // 8 bit periods (datasheet, 1.7 ms at 4800 baud) between sampling the data pin and the RF output; dropping the
  // carrier sooner would cut the end of the last byte off the air.
  uint32_t txLatencyHoldUs = 3000;
  uint32_t rxSettleUs = 3000;         // ignore received bytes this long after returning to receive
  uint32_t maxTxUs = 2000000;         // watchdog: longest the carrier may stay up
  uint32_t tripQuietUs = 1000000;     // after a watchdog trip, data keying stays off until the PC has been quiet this long
  uint32_t faultBackoffUs = 100000;   // after the chip fails to change state, wait before trying again
  uint32_t rtsHoldoffUs = 50000;      // after the port is opened or its baud rate changes, RTS may not key for this long
  uint32_t rxCheckUs = 20000;         // while receiving, ask the chip this often whether it is secretly transmitting
  bool echo = true;                   // send transmitted bytes back to the PC
};

class Modem {
 public:
  enum class State : uint8_t { kRx, kTxLead, kTxData, kTxTail };

  struct Stats {
    uint32_t keyUps = 0;
    uint32_t watchdogTrips = 0;
    uint32_t radioFaults = 0;         // the chip did not reach the requested state (any change, including retries)
    uint32_t keyUpFailures = 0;       // ... of which this many were failed attempts to start transmitting
    uint32_t unexpectedTx = 0;        // the chip said it was transmitting while the modem was receiving
    uint32_t hostBytesIn = 0;         // bytes the PC sent
    uint32_t txBytes = 0;             // bytes handed to the UART
    uint32_t txAborted = 0;           // ... of which this many were thrown away unsent when the carrier was cut
    uint32_t txDroppedNoKey = 0;      // PC bytes ignored because RTS was not up (key mode RTS) or was withdrawn
    uint32_t txDroppedFull = 0;       // PC bytes lost because the transmit buffer was full
    uint32_t txDroppedFault = 0;      // PC bytes lost: radio not ready, radio failed, watchdog, or watchdog hold-off
    uint32_t rxBytes = 0;             // radio bytes forwarded to the PC
    uint32_t rxDiscardedTx = 0;       // radio-side bytes ignored while transmitting
    uint32_t rxDiscardedSettle = 0;   // ... or just after returning to receive
    uint32_t hostDroppedNoDtr = 0;    // bytes for the PC dropped because it had not opened the port (DTR off)
    uint32_t hostDroppedFull = 0;     // ... or because the PC-bound buffer was full
  };

  Modem(const ModemConfig& cfg, IRadio& radio, IUart& uart, IHostLink& host, IClock& clock);

  // The modem cannot key the transmitter until Device::begin() has configured and verified the radio and says so.
  void setRadioReady(bool ready);
  bool radioReady() const { return radioReady_; }

  // The USB port was just opened, or its baud rate just changed: the host's RTS cannot be trusted for rtsHoldoffUs
  // (a terminal raises RTS at open, before the baud rate that says it is a console has arrived). Shorter than the ~80 ms
  // a host program waits between raising RTS and writing, so a real transmission is not affected.
  void holdRts();

  // Inputs from the outside world. None of them blocks except the chip state changes that tick() requests.
  void onLineState(bool dtr, bool rts);                    // USB serial control lines changed
  void onHostBytes(const uint8_t* data, size_t len);       // bytes from the PC
  void onUartRx(const uint8_t* data, size_t len);          // bytes the UART read from the chip's data pin
  void tick();                                             // call every loop; does all timing and key-up/down

  // Bench helpers used by the console. Both refuse (false) unless the radio is ready and idle in receive.
  // The carrier lasts the requested time plus the data-key lead and tail (so at least ~165 ms).
  bool carrierBurst(uint32_t durationUs);                  // unmodulated carrier (mark tone), then back to receive
  bool sendBytes(const uint8_t* data, size_t len);         // like PC bytes in key mode data, whatever the mode

  void setKeyMode(KeyMode m) { cfg_.keyMode = m; }
  KeyMode keyMode() const { return cfg_.keyMode; }
  const ModemConfig& config() const { return cfg_; }
  State state() const { return state_; }
  bool keyed() const { return state_ != State::kRx; }
  bool dtr() const { return dtr_; }
  bool rts() const { return rts_; }
  bool lockedOut() const { return lockout_ || dataLockout_; }
  const Stats& stats() const { return stats_; }

  static constexpr size_t kTxBufferSize = 1024;
  static constexpr size_t kHostBufferSize = 2048;

 private:
  bool rtsHolds() const { return rts_ && !lockout_ && !rtsHold_ && cfg_.keyMode != KeyMode::kData; }
  bool testActive() const;
  bool wantKey() const;
  void startTx();
  void stopTx();
  void forceRx(bool dropTxBytes);
  void pump();
  void pushHost(const uint8_t* data, size_t len);
  void flushHost();
  bool idleTx() const;
  void housekeeping();

  ModemConfig cfg_;
  IRadio& radio_;
  IUart& uart_;
  IHostLink& host_;
  IClock& clock_;

  State state_ = State::kRx;
  bool radioReady_ = false;
  bool dtr_ = false;
  bool rts_ = false;
  bool lockout_ = false;       // watchdog fired while RTS was up: ignore RTS until the PC lets go once
  bool dataLockout_ = false;   // watchdog fired: ignore PC bytes until the PC has been quiet for tripQuietUs
  bool keyedByRts_ = false;
  bool forceData_ = false;     // sendBytes(): key for data whatever the key mode
  bool testActive_ = false;
  bool needRxRecovery_ = false;
  bool faultBackoff_ = false;  // cleared by housekeeping() when its time is up, never by a later comparison
  bool settling_ = true;       // receive settle window open; likewise cleared by housekeeping()
  bool rtsHold_ = false;       // holdRts() is in effect; likewise cleared by housekeeping()
  uint32_t testUntil_ = 0;
  uint32_t keyedAt_ = 0;
  uint32_t tailStart_ = 0;
  uint32_t leadUs_ = 0;
  uint32_t tailUs_ = 0;
  uint32_t rxSince_ = 0;
  uint32_t faultAt_ = 0;
  uint32_t lastHostByteAt_ = 0;
  uint32_t lastRxCheckAt_ = 0;
  uint32_t rtsHoldAt_ = 0;

  RingBuffer<kTxBufferSize> txBuf_;
  RingBuffer<kHostBufferSize> hostOut_;
  Stats stats_;
};

}  // namespace openhsdu
