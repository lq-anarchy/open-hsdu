// open-hsdu firmware: an ESP32-S3 and a CC1101 standing in for the HSDU.
//
// Everything with logic in it lives in ../core and is tested on a PC. This file only connects that core to
// the ESP32's SPI, UART and USB, and runs its loop. Read docs/ARCHITECTURE.md first.
//
// USB: ONE serial port (the Arduino-ESP32 framework supports only one). While the host has it at 115200 baud (TinyUSB's
// power-up default, and what a terminal uses) it is the text console (`help`); at any other baud rate it is the HSDU
// data port: bytes in are transmitted, RTS keys the transmitter, received bytes and the echo come out. The baud rate and
// the DTR/RTS lines are read from TinyUSB itself on every pass. See docs/SERIAL-INTERFACE.md.
#include <Arduino.h>
#include <SPI.h>
#include <USB.h>
#include <Preferences.h>
#include <USBCDC.h>

#include "driver/uart.h"
#include "esp32-hal-tinyusb.h"
#include "esp_task_wdt.h"
#include "hal/uart_ll.h"

#include "openhsdu/cc1101.h"
#include "openhsdu/console.h"
#include "openhsdu/device.h"
#include "openhsdu/hal.h"
#include "openhsdu/host_mux.h"
#include "openhsdu/modem.h"
#include "openhsdu/power.h"
#include "openhsdu/profiles.h"
#include "openhsdu/version.h"
#include "pins.h"

#ifndef OPENHSDU_UART_NUM
#define OPENHSDU_UART_NUM 1
#endif

namespace {

using namespace openhsdu;

constexpr uint32_t kUartBaud = 4800;
constexpr uint32_t kSpiHz = 4000000;  // datasheet Table 22: 6.5 MHz for burst access, so 4 MHz is well inside
constexpr uint32_t kLoopWatchdogSeconds = 1;
#ifdef OPENHSDU_ALLOW_HIGH_POWER
constexpr double kMaxChipDbm = 10.0;
#else
constexpr double kMaxChipDbm = kDefaultMaxChipDbm;
#endif

USBCDC dataPort(0);
SPIClass spiBus(FSPI);

class EspSpi : public ISpi {
 public:
  void select(bool active) override {
    if (active && !inTransaction_) {
      spiBus.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
      inTransaction_ = true;
      digitalWrite(pins::kCsn, LOW);
    } else if (!active) {
      digitalWrite(pins::kCsn, HIGH);
      if (inTransaction_) spiBus.endTransaction();
      inTransaction_ = false;
    }
  }
  uint8_t transfer(uint8_t out) override { return spiBus.transfer(out); }
  bool misoLow() override { return digitalRead(pins::kMiso) == LOW; }

 private:
  bool inTransaction_ = false;
};

class EspClock : public IClock {
 public:
  uint32_t micros() override { return static_cast<uint32_t>(::micros()); }
  void delayUs(uint32_t us) override { delayMicroseconds(us); }
};

// UART1 on GPIO8 (TX, to the chip's GDO0) and GPIO9 (RX, from GDO2). Transmit goes straight into the hardware
// FIFO so nothing here ever blocks; receive uses the driver's ring buffer.
class EspUart : public IUart {
 public:
  void begin() {
    const uart_port_t port = static_cast<uart_port_t>(OPENHSDU_UART_NUM);
    uart_config_t c;
    memset(&c, 0, sizeof(c));
    c.baud_rate = kUartBaud;
    c.data_bits = UART_DATA_8_BITS;
    c.parity = UART_PARITY_DISABLE;
    c.stop_bits = UART_STOP_BITS_1;
    c.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    c.source_clk = UART_SCLK_APB;
    uart_driver_install(port, 1024, 0, 0, nullptr, 0);
    uart_param_config(port, &c);
    // Hand every received byte to the ring buffer at once: the driver's defaults (a 120-byte threshold and a
    // 100-bit-time timeout, 21 ms at 4800 baud) would deliver bytes late and in batches.
    uart_set_rx_full_threshold(port, 1);
    uart_set_rx_timeout(port, 1);
    // The transmit pin stays unconnected until connectTx(): GPIO8 must not drive the chip's GDO0 while the chip is
    // still at its power-on defaults (GDO0 is an output then).
    uart_set_pin(port, UART_PIN_NO_CHANGE, pins::kGdo2Rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  }
  void connectTx() {
    uart_set_pin(static_cast<uart_port_t>(OPENHSDU_UART_NUM), pins::kGdo0Tx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE);
  }
  size_t write(const uint8_t* data, size_t len) override {
    uart_dev_t* hw = UART_LL_GET_HW(OPENHSDU_UART_NUM);
    const size_t room = uart_ll_get_txfifo_len(hw);
    const size_t n = len < room ? len : room;
    if (n > 0) uart_ll_write_txfifo(hw, data, static_cast<uint32_t>(n));
    return n;
  }
  size_t writable() override { return uart_ll_get_txfifo_len(UART_LL_GET_HW(OPENHSDU_UART_NUM)); }
  bool busy() override { return !uart_ll_is_tx_idle(UART_LL_GET_HW(OPENHSDU_UART_NUM)); }
  void flushRx() override { uart_flush_input(static_cast<uart_port_t>(OPENHSDU_UART_NUM)); }
  size_t abortTx() override {
    uart_dev_t* hw = UART_LL_GET_HW(OPENHSDU_UART_NUM);
    const size_t queued = UART_LL_FIFO_DEF_LEN - uart_ll_get_txfifo_len(hw);
    uart_ll_txfifo_rst(hw);  // a byte already in the shift register still finishes
    return queued;
  }
  size_t read(uint8_t* out, size_t len) {
    const int n = uart_read_bytes(static_cast<uart_port_t>(OPENHSDU_UART_NUM), out, static_cast<uint32_t>(len), 0);
    return n > 0 ? static_cast<size_t>(n) : 0;
  }
};

// Never blocks: writes what fits in the USB queue and reports how much that was.
class UsbHost : public IHostLink {
 public:
  size_t write(const uint8_t* data, size_t len) override {
    const size_t room = dataPort.availableForWrite();
    const size_t n = len < room ? len : room;
    return n > 0 ? dataPort.write(data, n) : 0;
  }
};

EspSpi gSpi;
EspClock gClock;
EspUart gUart;
UsbHost gHost;
Cc1101 gRadio(gSpi, gClock);
// Firmware default: key by RTS only, like the real HSDU. Bytes alone never transmit, so no program or operating-system
// service that opens the port and writes (a modem probe, say) can put anything on the air. `keymode either` or `data`
// is the documented fallback if RTS does not reach the firmware; the choice is stored in flash.
ModemConfig makeConfig() {
  ModemConfig c;
  c.keyMode = KeyMode::kRts;
  return c;
}
Modem gModem(makeConfig(), gRadio, gUart, gHost, gClock);
Device gDevice(gRadio, gModem);
HostMux gMux(gModem, gHost);
Console gConsole(gDevice, gModem, gMux);

bool gLastDtr = false;
bool gLastRts = false;
uint32_t gLastBaud = 0xFFFFFFFFu;

Preferences gPrefs;
KeyMode gSavedMode = KeyMode::kRts;

uint8_t encodeMode(KeyMode m) { return m == KeyMode::kData ? 1 : m == KeyMode::kEither ? 2 : 0; }

void loadKeyMode() {
  gPrefs.begin("openhsdu", true);
  const uint8_t v = gPrefs.getUChar("keymode", 0);
  gPrefs.end();
  gSavedMode = v == 1 ? KeyMode::kData : v == 2 ? KeyMode::kEither : KeyMode::kRts;
  gModem.setKeyMode(gSavedMode);
}

// Flash is written only when the console changed the mode, and never while transmitting (a flash write stalls the CPU).
void saveKeyModeIfChanged() {
  if (gModem.keyMode() == gSavedMode || gModem.keyed()) return;
  gPrefs.begin("openhsdu", false);
  gPrefs.putUChar("keymode", encodeMode(gModem.keyMode()));
  gPrefs.end();
  gSavedMode = gModem.keyMode();
}

// DTR and RTS are polled from TinyUSB rather than taken from events, so the unplugged and suspended cases are
// covered by the same line: when the bus is not ready both read as low, which drops RTS and ends any carrier.
void pollUsbState() {
  const bool ready = tud_ready();
  const uint8_t lines = ready ? tud_cdc_n_get_line_state(0) : 0;
  const bool dtr = (lines & 0x01) != 0;
  const bool rts = (lines & 0x02) != 0;
  // TinyUSB's stored coding is updated on every SET_LINE_CODING (the framework's event for it is filtered to changes
  // only, which misses a second session at the same baud rate). Not ready means unplugged or suspended: baud 0, lines low.
  cdc_line_coding_t coding;
  tud_cdc_n_get_line_coding(0, &coding);
  const uint32_t baud = ready ? coding.bit_rate : 0;
  if (baud != gLastBaud) {
    gMux.onLineCoding(baud);
    gLastBaud = baud;
  }
  if (dtr != gLastDtr || rts != gLastRts) {
    gMux.onLineState(dtr, rts);
    gLastDtr = dtr;
    gLastRts = rts;
  }
}

}  // namespace

void setup() {
  pinMode(pins::kCsn, OUTPUT);
  digitalWrite(pins::kCsn, HIGH);
  pinMode(pins::kMiso, INPUT);
  spiBus.begin(pins::kSck, pins::kMiso, pins::kMosi, -1);  // chip select is driven by hand, see EspSpi

  gMux.setConsole(&gConsole);
  gUart.begin();

  dataPort.enableReboot(false);  // a DTR/RTS pattern must never reboot the board into its loader
  dataPort.setRxBufferSize(2048);
  dataPort.begin();
  USB.productName("open-hsdu");
  USB.manufacturerName("open-hsdu");
  USB.begin();

  loadKeyMode();

  // The modem cannot key until begin() succeeds; if it fails, `status` over the console says radio=MISSING.
  if (gDevice.begin(profileProven(), kDefaultPatable, kMaxChipDbm)) gUart.connectTx();

  // A hardware backstop for the software watchdog in the modem: if loop() ever stops running, the chip resets
  // (the CC1101 itself keeps transmitting until the new boot resets it, about half a second).
  esp_task_wdt_init(kLoopWatchdogSeconds, true);
  enableLoopWDT();
}

void loop() {
  pollUsbState();

  uint8_t buf[128];
  int avail;
  while ((avail = dataPort.available()) > 0) {
    const size_t want = static_cast<size_t>(avail) < sizeof(buf) ? static_cast<size_t>(avail) : sizeof(buf);
    const size_t n = dataPort.readBytes(buf, want);
    if (n == 0) break;
    gMux.onHostBytes(buf, n);
  }

  const size_t got = gUart.read(buf, sizeof(buf));
  if (got > 0) gModem.onUartRx(buf, got);

  gModem.tick();
  gMux.poll();
  saveKeyModeIfChanged();
  yield();
}
