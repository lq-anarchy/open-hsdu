// Wiring: ESP32-S3-Zero to the CC1101 module (docs/HARDWARE.md). Plain constants, so the host tests can check
// them without the Arduino toolchain.
#pragma once

namespace pins {

constexpr int kCsn = 10;
constexpr int kMosi = 11;
constexpr int kSck = 12;
constexpr int kMiso = 13;
constexpr int kGdo0Tx = 8;  // UART1 TX -> chip GDO0 (data in while transmitting)
constexpr int kGdo2Rx = 9;  // UART1 RX <- chip GDO2 (demodulated data out)

// GPIOs that must not be used: boot straps (0, 3, 45, 46), the USB pins (19, 20), the on-board RGB LED (21).
constexpr int kForbidden[] = {0, 3, 19, 20, 21, 45, 46};

constexpr int kAll[] = {kCsn, kMosi, kSck, kMiso, kGdo0Tx, kGdo2Rx};

}  // namespace pins
