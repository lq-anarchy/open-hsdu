#include "openhsdu/device.h"

#include "openhsdu/cc1101_regs.h"
#include "openhsdu/power.h"

namespace openhsdu {

using namespace cc1101;

namespace {

bool powerAllowed(uint8_t patable, double maxChipDbm) {
  const PowerStep* s = findPowerStep(patable);
  return s != nullptr && s->chipDbm <= maxChipDbm;
}

// Which bits of a register are still the values we wrote after the chip has calibrated. FSCAL3[3:0], FSCAL2[4:0]
// and FSCAL1[5:0] hold calibration results that the chip overwrites on every IDLE -> RX/TX calibration.
uint8_t readbackMask(uint8_t addr) {
  switch (addr) {
    case FSCAL3: return 0xF0;
    case FSCAL2: return 0x20;
    case FSCAL1: return 0x00;
    default: return 0xFF;
  }
}

}  // namespace

bool Device::begin(const Profile& profile, uint8_t patable, double maxChipDbm) {
  if (modem_.keyed()) return false;  // never reset the chip under a live carrier
  began_ = false;
  profile_ = &profile;
  patable_ = patable;
  maxChipDbm_ = maxChipDbm;
  present_ = false;
  modem_.setRadioReady(false);
  if (!powerAllowed(patable, maxChipDbm)) return false;  // never start above the cap
  if (!radio_.reset()) return false;
  if (!radio_.detect()) return false;
  if (!radio_.apply(profile, patable).ok) return false;
  if (!radio_.enterRx()) return false;
  present_ = true;
  began_ = true;
  modem_.setRadioReady(true);
  return true;
}

bool Device::setProfile(const Profile& profile) {
  if (!began_ || modem_.keyed() || !powerAllowed(patable_, maxChipDbm_)) return false;  // never before a successful begin()
  const Profile* previous = profile_;
  modem_.setRadioReady(false);
  present_ = false;
  if (!radio_.detect()) return false;  // not a CC1101 (or not answering): leave it alone
  if (!radio_.apply(profile, patable_).ok) {
    // Put back what was there, so the chip is not left half-configured; the modem stays disabled if even that fails.
    if (previous != nullptr && previous != &profile && radio_.apply(*previous, patable_).ok && radio_.enterRx()) {
      present_ = true;
      modem_.setRadioReady(true);
    }
    return false;
  }
  profile_ = &profile;
  if (!radio_.enterRx()) return false;
  present_ = true;
  modem_.setRadioReady(true);
  return true;
}

bool Device::setPatable(uint8_t patable) {
  if (!present_ || modem_.keyed() || !powerAllowed(patable, maxChipDbm_)) return false;
  radio_.writePatable(patable);
  if (radio_.readPatable0() != patable) {
    // The power setting is unknown: do not allow transmitting until a later change succeeds.
    present_ = false;
    modem_.setRadioReady(false);
    return false;
  }
  patable_ = patable;
  return true;
}

bool Device::selfTest(SelfTestReport& out) {
  out = SelfTestReport();
  if (modem_.keyed()) return false;
  out.detected = radio_.detect(&out.part, &out.version);
  if (!out.detected || profile_ == nullptr) return true;
  for (size_t i = 0; i < profile_->count; ++i) {
    const uint8_t addr = profile_->regs[i].addr;
    const uint8_t mask = readbackMask(addr);
    if (mask != 0 && ((radio_.readReg(addr) ^ profile_->regs[i].value) & mask) != 0) {
      if (out.mismatches == 0) out.firstBadAddr = addr;
      if (out.mismatches < 255) ++out.mismatches;
    }
  }
  if ((radio_.readReg(FSCAL1) & 0x3F) == 0x3F) {  // datasheet 22.1: the synthesizer did not lock
    if (out.mismatches == 0) out.firstBadAddr = FSCAL1;
    if (out.mismatches < 255) ++out.mismatches;
  }
  out.patableOk = radio_.readPatable0() == patable_;
  out.marc = radio_.marcState();
  out.inRx = out.marc == kMarcRx;
  out.pass = out.detected && out.mismatches == 0 && out.patableOk && out.inRx;
  return true;
}

}  // namespace openhsdu
