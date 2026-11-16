// CC1101 driver: SPI access, reset, identification, applying a register profile with read-back, and the two
// state changes the modem needs (transmit, receive). Written against ISpi/IClock so it runs on a model of the
// chip in the tests.
#pragma once
#include <cstddef>
#include <cstdint>

#include "openhsdu/cc1101_regs.h"
#include "openhsdu/hal.h"
#include "openhsdu/profiles.h"

namespace openhsdu {

struct ApplyResult {
  bool ok = false;
  uint8_t mismatches = 0;    // registers whose read-back differed from what was written
  uint8_t firstBadAddr = 0;  // address of the first mismatch
  bool patableOk = false;
};

class Cc1101 : public IRadio {
 public:
  Cc1101(ISpi& spi, IClock& clock) : spi_(spi), clock_(clock) {}

  // Datasheet manual-reset sequence: CSN pulse, wait for MISO low, SRES, wait for MISO low. False on timeout.
  bool reset();
  // PARTNUM must be 0x00 and VERSION 0x14 (current) or 0x04 (old). Fills the two bytes for the caller.
  bool detect(uint8_t* partNum = nullptr, uint8_t* version = nullptr);

  // Writes every register of the profile, then PATABLE[0], then reads them all back. The chip is left idle.
  ApplyResult apply(const Profile& profile, uint8_t patable);

  // IRadio. Both go through IDLE first (so a pending clear-channel check can never block a transmit) and
  // wait, with a timeout, for the chip to report the new state.
  bool enterTx() override;
  bool enterRx() override;
  bool enterIdle();
  bool transmitting() override { return marcState() == cc1101::kMarcTx; }
  // Received signal strength in dBm from the RSSI status register, using the datasheet's offset of about 74 dB
  // at 433 MHz (approximate; the chip must be receiving).
  double rssiDbm();
  static double rssiDbmFromRaw(uint8_t raw);

  // Raw access, used by the console and the self-test.
  uint8_t readReg(uint8_t addr);
  void writeReg(uint8_t addr, uint8_t value);
  uint8_t readStatus(uint8_t addr);  // read until two reads agree (CC1101 errata: status reads can glitch)
  uint8_t marcState();
  uint8_t strobe(uint8_t cmd);       // returns the chip status byte, 0xFF if the chip never became ready
  void writePatable(uint8_t value);  // 8-byte burst: entry 0 = value, the rest 0
  uint8_t readPatable0();

  uint32_t timeouts() const { return timeouts_; }

 private:
  bool waitReady();
  bool waitState(uint8_t marc, uint32_t timeoutUs);

  ISpi& spi_;
  IClock& clock_;
  uint32_t timeouts_ = 0;
};

}  // namespace openhsdu
