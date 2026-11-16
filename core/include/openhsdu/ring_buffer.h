// Fixed-size byte FIFO with no heap use. Not thread-safe: the firmware feeds and drains it from one loop.
#pragma once
#include <cstddef>
#include <cstdint>

namespace openhsdu {

template <size_t N>
class RingBuffer {
  static_assert(N >= 2, "ring buffer needs at least two bytes");

 public:
  size_t size() const { return count_; }
  size_t capacity() const { return N; }
  size_t free() const { return N - count_; }
  bool empty() const { return count_ == 0; }
  void clear() { head_ = tail_ = count_ = 0; }

  // Appends as many bytes as fit; returns how many were stored.
  size_t push(const uint8_t* data, size_t len) {
    size_t n = len < free() ? len : free();
    for (size_t i = 0; i < n; ++i) {
      buf_[tail_] = data[i];
      tail_ = (tail_ + 1) % N;
    }
    count_ += n;
    return n;
  }

  // Copies up to `len` bytes out without removing them; returns the count copied.
  size_t peek(uint8_t* out, size_t len) const {
    size_t n = len < count_ ? len : count_;
    size_t h = head_;
    for (size_t i = 0; i < n; ++i) {
      out[i] = buf_[h];
      h = (h + 1) % N;
    }
    return n;
  }

  void drop(size_t len) {
    size_t n = len < count_ ? len : count_;
    head_ = (head_ + n) % N;
    count_ -= n;
  }

 private:
  uint8_t buf_[N] = {};
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t count_ = 0;
};

}  // namespace openhsdu
