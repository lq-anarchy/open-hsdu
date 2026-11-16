#include <vector>

#include "doctest.h"
#include "openhsdu/ring_buffer.h"

using openhsdu::RingBuffer;

TEST_SUITE("ring_buffer") {

TEST_CASE("push, peek, drop keep order") {
  RingBuffer<8> r;
  const uint8_t in[5] = {1, 2, 3, 4, 5};
  CHECK(r.push(in, 5) == 5);
  uint8_t out[8] = {};
  CHECK(r.peek(out, 8) == 5);
  CHECK(out[0] == 1);
  CHECK(out[4] == 5);
  r.drop(2);
  CHECK(r.size() == 3);
  CHECK(r.peek(out, 1) == 1);
  CHECK(out[0] == 3);
}

TEST_CASE("push stops at capacity and reports how many fit") {
  RingBuffer<4> r;
  const uint8_t in[6] = {1, 2, 3, 4, 5, 6};
  CHECK(r.push(in, 6) == 4);
  CHECK(r.free() == 0);
  CHECK(r.push(in, 1) == 0);
}

TEST_CASE("wraps around the end of the storage") {
  RingBuffer<4> r;
  const uint8_t a[3] = {1, 2, 3};
  r.push(a, 3);
  r.drop(2);
  const uint8_t b[3] = {4, 5, 6};
  CHECK(r.push(b, 3) == 3);
  uint8_t out[4] = {};
  CHECK(r.peek(out, 4) == 4);
  CHECK(out[0] == 3);
  CHECK(out[1] == 4);
  CHECK(out[2] == 5);
  CHECK(out[3] == 6);
}

TEST_CASE("drop and peek are clamped to what is stored") {
  RingBuffer<4> r;
  const uint8_t a[2] = {7, 8};
  r.push(a, 2);
  r.drop(10);
  CHECK(r.empty());
  uint8_t out[4];
  CHECK(r.peek(out, 4) == 0);
}

TEST_CASE("clear empties it and it can be refilled") {
  RingBuffer<4> r;
  const uint8_t a[4] = {1, 2, 3, 4};
  r.push(a, 4);
  r.clear();
  CHECK(r.empty());
  CHECK(r.push(a, 4) == 4);
}

TEST_CASE("many cycles through a small buffer never corrupt the order") {
  RingBuffer<7> r;
  uint8_t next = 0, expect = 0;
  for (int i = 0; i < 5000; ++i) {
    const uint8_t b = next;
    if (r.push(&b, 1) == 1) ++next;
    if (i % 3 != 0) {
      uint8_t out;
      if (r.peek(&out, 1) == 1) {
        CHECK(out == expect);
        ++expect;
        r.drop(1);
      }
    }
  }
}

}  // TEST_SUITE
