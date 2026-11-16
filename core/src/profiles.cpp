#include "openhsdu/profiles.h"

#include <cstring>

#include "openhsdu/cc1101_regs.h"

namespace openhsdu {

using namespace cc1101;

namespace {

constexpr RegSource P = RegSource::kProven;
constexpr RegSource B = RegSource::kBareModule;
constexpr RegSource S = RegSource::kSmartRf;

// Registers in address order. Order does not matter to the chip, but a fixed order keeps dumps comparable.
constexpr RegPair kProvenRegs[] = {
    {IOCFG2, kIocfgSerialDataOut, B},  // GDO2: demodulated data out, wired to the UART receive pin
    {IOCFG0, kIocfgHighZ, B},          // GDO0: left floating so the UART transmit pin can drive it
    {PKTCTRL1, 0x04, P},
    {PKTCTRL0, 0x32, P},               // PKT_FORMAT = 3: asynchronous serial mode, infinite length, no CRC
    {FSCTRL1, 0x06, P},
    {FREQ2, 0x10, B},                  // 418.000 MHz; the Flipper preset has no frequency registers
    {FREQ1, 0x13, B},
    {FREQ0, 0xB1, B},
    {MDMCFG4, 0x67, P},                // channel filter 270.8 kHz, data-rate exponent 7
    {MDMCFG3, 0x83, P},                // data-rate mantissa: 4,798 baud
    {MDMCFG2, 0x04, P},                // 2-FSK
    {MDMCFG1, 0x02, P},
    {MDMCFG0, 0x00, P},
    {DEVIATN, 0x47, P},                // +/-47.6 kHz
    {MCSM1, 0x00, B},                  // clear-channel check off; return to idle after receive and transmit
    {MCSM0, 0x18, P},                  // calibrate on every idle -> receive/transmit
    {FOCCFG, 0x16, P},                 // frequency-offset compensation ON, limit BW/4
    {AGCCTRL2, 0x07, P},
    {AGCCTRL1, 0x00, P},
    {AGCCTRL0, 0x91, P},
    {WORCTRL, 0xFB, P},
    {FREND1, 0x56, P},
    {FREND0, 0x10, P},
    {FSCAL3, 0xE9, S},
    {FSCAL2, 0x2A, S},
    {FSCAL1, 0x00, S},
    {FSCAL0, 0x1F, S},
    {TEST2, 0x81, S},
    {TEST1, 0x35, S},
    {TEST0, 0x09, S},
};

// Same as the proven set with two differences: the carrier is 418.012 MHz (FREQ0 0xCF) and the deviation is
// the +/-41.3 kHz step nearest to the HSDU's measured +/-42.5 kHz (DEVIATN 0x45).
constexpr RegPair kMatchedRegs[] = {
    {IOCFG2, kIocfgSerialDataOut, B},
    {IOCFG0, kIocfgHighZ, B},
    {PKTCTRL1, 0x04, P},
    {PKTCTRL0, 0x32, P},
    {FSCTRL1, 0x06, P},
    {FREQ2, 0x10, B},
    {FREQ1, 0x13, B},
    {FREQ0, 0xCF, B},
    {MDMCFG4, 0x67, P},
    {MDMCFG3, 0x83, P},
    {MDMCFG2, 0x04, P},
    {MDMCFG1, 0x02, P},
    {MDMCFG0, 0x00, P},
    {DEVIATN, 0x45, B},
    {MCSM1, 0x00, B},
    {MCSM0, 0x18, P},
    {FOCCFG, 0x16, P},
    {AGCCTRL2, 0x07, P},
    {AGCCTRL1, 0x00, P},
    {AGCCTRL0, 0x91, P},
    {WORCTRL, 0xFB, P},
    {FREND1, 0x56, P},
    {FREND0, 0x10, P},
    {FSCAL3, 0xE9, S},
    {FSCAL2, 0x2A, S},
    {FSCAL1, 0x00, S},
    {FSCAL0, 0x1F, S},
    {TEST2, 0x81, S},
    {TEST1, 0x35, S},
    {TEST0, 0x09, S},
};

const Profile kProven = {"proven",  kProvenRegs,  sizeof(kProvenRegs) / sizeof(kProvenRegs[0]),
                         418.000e6, 4800.0,       47.6e3, 270.8e3};
const Profile kMatched = {"matched", kMatchedRegs, sizeof(kMatchedRegs) / sizeof(kMatchedRegs[0]),
                          418.012e6, 4800.0,       41.3e3, 270.8e3};

}  // namespace

const Profile& profileProven() { return kProven; }
const Profile& profileHsduMatched() { return kMatched; }

const Profile* profileByName(const char* name) {
  if (name == nullptr) return nullptr;
  if (std::strcmp(name, kProven.name) == 0) return &kProven;
  if (std::strcmp(name, kMatched.name) == 0) return &kMatched;
  return nullptr;
}

}  // namespace openhsdu
