#include "openhsdu/host_mux.h"

namespace openhsdu {

void HostMux::applyLines() {
  // In console mode the modem sees a closed port: no RTS to key it, no DTR to send it anything.
  const bool data = !consoleMode();
  modem_.onLineState(data && dtr_, data && rts_);
}

void HostMux::onLineCoding(uint32_t baud) {
  const bool was = consoleMode();
  const bool changed = baud != baud_;
  baud_ = baud;
  if (changed) modem_.holdRts();  // the mode may be about to change; do not trust RTS until it has settled
  if (was != consoleMode()) {
    out_.clear();  // text for one mode must never appear in the other
    if (console_ != nullptr) console_->reset();
    applyLines();
  }
}

void HostMux::onLineState(bool dtr, bool rts) {
  if (dtr && !dtr_) modem_.holdRts();  // the port was just opened: a terminal raises RTS before it sets the baud rate
  if (!dtr && dtr_ && console_ != nullptr) console_->reset();
  dtr_ = dtr;
  rts_ = rts;
  if (!dtr) out_.clear();
  applyLines();
}

void HostMux::onHostBytes(const uint8_t* data, size_t len) {
  if (consoleMode()) {
    if (console_ != nullptr) console_->feed(data, len);
  } else {
    modem_.onHostBytes(data, len);
  }
}

void HostMux::write(const char* text, size_t len) {
  if (!consoleMode() || !dtr_) return;
  out_.push(reinterpret_cast<const uint8_t*>(text), len);
}

void HostMux::poll() {
  uint8_t tmp[64];
  while (consoleMode() && dtr_ && !out_.empty()) {
    const size_t n = out_.peek(tmp, sizeof(tmp));
    const size_t w = host_.write(tmp, n);
    out_.drop(w);
    if (w < n) break;
  }
}

}  // namespace openhsdu
