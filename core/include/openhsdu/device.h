// The radio side of the device: boot sequence, the active profile and power step, the power policy, and the
// self-test. The console and the firmware's setup() talk to this, not to the chip.
#pragma once
#include <cstdint>

#include "openhsdu/cc1101.h"
#include "openhsdu/modem.h"
#include "openhsdu/profiles.h"

namespace openhsdu {

struct SelfTestReport {
  bool detected = false;     // PARTNUM and VERSION are those of a CC1101
  uint8_t part = 0;
  uint8_t version = 0;
  uint8_t mismatches = 0;    // configuration registers that read back differently from the profile
  uint8_t firstBadAddr = 0;
  bool patableOk = false;
  uint8_t marc = 0;
  bool inRx = false;
  bool pass = false;
};

class Device {
 public:
  Device(Cc1101& radio, Modem& modem) : radio_(radio), modem_(modem) {}

  // Reset the chip, check it is a CC1101, apply the profile and power step, start receiving, and only then tell the
  // modem it may key. Returns false (and leaves the modem unable to key) if the chip is missing or does not take its
  // settings. The modem starts out unable to key, so a failure here can never produce a transmission.
  bool begin(const Profile& profile, uint8_t patable, double maxChipDbm);

  bool present() const { return present_; }
  const Profile& profile() const { return *profile_; }
  uint8_t patable() const { return patable_; }
  double maxChipDbm() const { return maxChipDbm_; }

  // Each refuses (false) while the transmitter is keyed. If a change fails part-way the modem is disabled until a
  // later change (or a reboot) succeeds, so the radio is never keyed in a half-configured state.
  // setProfile: on failure the previous profile is put back if possible; it also works as a retry after a failure.
  bool setProfile(const Profile& profile);
  // Refuses a PATABLE byte that is not in the allowed table or whose nominal output is above the cap.
  bool setPatable(uint8_t patable);
  // Reads the chip back. FSCAL3/2/1 are calibration RESULT registers that the chip rewrites on every calibration, so
  // only their input bits are compared (and FSCAL1 only for the datasheet's "PLL not locked" value).
  bool selfTest(SelfTestReport& out);

  Cc1101& radio() { return radio_; }

 private:
  Cc1101& radio_;
  Modem& modem_;
  const Profile* profile_ = &profileProven();
  uint8_t patable_ = 0;
  double maxChipDbm_ = 0;
  bool present_ = false;
  bool began_ = false;  // a begin() has completed successfully since the last begin() started
};

}  // namespace openhsdu
