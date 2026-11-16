// CC1101 register map, command strobes, status registers and MARCSTATE values.
// Source: Texas Instruments CC1101 datasheet (SWRS061), register overview tables.
#pragma once
#include <cstdint>

namespace openhsdu {
namespace cc1101 {

// Configuration registers, 0x00..0x2E. Written one byte at a time, read back to verify.
enum Reg : uint8_t {
  IOCFG2 = 0x00, IOCFG1 = 0x01, IOCFG0 = 0x02, FIFOTHR = 0x03, SYNC1 = 0x04, SYNC0 = 0x05,
  PKTLEN = 0x06, PKTCTRL1 = 0x07, PKTCTRL0 = 0x08, ADDR = 0x09, CHANNR = 0x0A,
  FSCTRL1 = 0x0B, FSCTRL0 = 0x0C, FREQ2 = 0x0D, FREQ1 = 0x0E, FREQ0 = 0x0F,
  MDMCFG4 = 0x10, MDMCFG3 = 0x11, MDMCFG2 = 0x12, MDMCFG1 = 0x13, MDMCFG0 = 0x14,
  DEVIATN = 0x15, MCSM2 = 0x16, MCSM1 = 0x17, MCSM0 = 0x18, FOCCFG = 0x19, BSCFG = 0x1A,
  AGCCTRL2 = 0x1B, AGCCTRL1 = 0x1C, AGCCTRL0 = 0x1D, WOREVT1 = 0x1E, WOREVT0 = 0x1F,
  WORCTRL = 0x20, FREND1 = 0x21, FREND0 = 0x22, FSCAL3 = 0x23, FSCAL2 = 0x24, FSCAL1 = 0x25,
  FSCAL0 = 0x26, RCCTRL1 = 0x27, RCCTRL0 = 0x28, FSTEST = 0x29, PTEST = 0x2A, AGCTEST = 0x2B,
  TEST2 = 0x2C, TEST1 = 0x2D, TEST0 = 0x2E
};
constexpr uint8_t kNumConfigRegs = 0x2F;

// Command strobes, 0x30..0x3D (a header byte with no data).
enum Strobe : uint8_t {
  SRES = 0x30, SFSTXON = 0x31, SXOFF = 0x32, SCAL = 0x33, SRX = 0x34, STX = 0x35, SIDLE = 0x36,
  SWOR = 0x38, SPWD = 0x39, SFRX = 0x3A, SFTX = 0x3B, SWORRST = 0x3C, SNOP = 0x3D
};

// Status registers share addresses 0x30..0x3D with the strobes; the burst bit selects the status register.
enum Status : uint8_t {
  PARTNUM = 0x30, VERSION = 0x31, FREQEST = 0x32, LQI = 0x33, RSSI = 0x34, MARCSTATE = 0x35,
  WORTIME1 = 0x36, WORTIME0 = 0x37, PKTSTATUS = 0x38, VCO_VC_DAC = 0x39, TXBYTES = 0x3A, RXBYTES = 0x3B
};

constexpr uint8_t PATABLE = 0x3E;  // power table, 8 bytes, burst access
constexpr uint8_t FIFO = 0x3F;

// SPI header byte: bit 7 = read, bit 6 = burst.
constexpr uint8_t kHeaderRead = 0x80;
constexpr uint8_t kHeaderBurst = 0x40;

// MARCSTATE (status register 0x35), low 5 bits.
enum MarcState : uint8_t {
  kMarcSleep = 0x00, kMarcIdle = 0x01, kMarcXoff = 0x02, kMarcVcoonMc = 0x03, kMarcRegonMc = 0x04,
  kMarcMancal = 0x05, kMarcVcoon = 0x06, kMarcRegon = 0x07, kMarcStartcal = 0x08,
  kMarcBwboost = 0x09, kMarcFsLock = 0x0A, kMarcIfadcon = 0x0B, kMarcEndcal = 0x0C,
  kMarcRx = 0x0D, kMarcRxEnd = 0x0E, kMarcRxRst = 0x0F, kMarcTxrxSwitch = 0x10,
  kMarcRxfifoOverflow = 0x11, kMarcFstxon = 0x12, kMarcTx = 0x13, kMarcTxEnd = 0x14,
  kMarcRxtxSwitch = 0x15, kMarcTxfifoUnderflow = 0x16
};

// Chip identification. PARTNUM is 0x00; VERSION is 0x14 on current silicon and 0x04 on older parts.
constexpr uint8_t kPartNum = 0x00;
constexpr uint8_t kVersionCurrent = 0x14;
constexpr uint8_t kVersionOld = 0x04;

// Bit fields used by the code and the tests.
constexpr uint8_t kPktFormatMask = 0x30;       // PKTCTRL0 bits 5:4
constexpr uint8_t kPktFormatAsyncSerial = 0x30;  // PKT_FORMAT = 3, asynchronous serial mode
constexpr uint8_t kModFormatMask = 0x70;       // MDMCFG2 bits 6:4
constexpr uint8_t kModFormat2Fsk = 0x00;
constexpr uint8_t kCcaModeMask = 0x30;         // MCSM1 bits 5:4
constexpr uint8_t kIocfgHighZ = 0x2E;          // pin tri-stated (used for GDO0 so the UART can drive it)
constexpr uint8_t kIocfgSerialDataOut = 0x0D;  // pin carries the demodulated serial data

}  // namespace cc1101
}  // namespace openhsdu
