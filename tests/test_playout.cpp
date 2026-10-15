#include "playout.hpp"
#include <cassert>
#include <climits>

int main() {
  live_audio::Prebuffer b;
  int16_t frame[live_audio::Prebuffer::MAX_PACKET];
  for (size_t i = 0; i < sizeof(frame) / sizeof(frame[0]); ++i)
    frame[i] = int16_t(i);
  assert(!b.ready(0, true));
  assert(!b.append(nullptr, 480, 0));
  assert(!b.append(frame, 0, 0));
  assert(!b.append(frame, sizeof(frame) / sizeof(frame[0]) + 1, 0));
  for (unsigned i = 0; i < 6; ++i) {
    for (unsigned j = 0; j < 480; ++j)
      frame[j] = int16_t(i * 480 + j);
    assert(b.append(frame, 480, i * 20));
    assert(b.ready(i * 20, true) == (i == 5));
    assert(b.ready(i * 20, false));
  }
  assert(b.size == 2880);
  for (size_t i = 0; i < b.size; ++i)
    assert(b.pcm[i] == int16_t(i));
  b.clear();
  // Short final packets cannot wait forever for a larger buffer. Millisecond
  // wrap must not release early or turn the bounded wait into a huge delay.
  assert(b.append(frame, 60, UINT32_MAX - 30));
  assert(!b.ready(88, true));
  assert(b.ready(89, true));
  assert(b.size == 60);
  b.clear();
  // Accept the largest legal packet just below the target without overrun.
  assert(b.append(frame, 2879, 0));
  assert(b.append(frame, 2880, 20));
  assert(b.size == 5759 && b.ready(20, true));
  assert(!b.append(frame, 2, 20));
  assert(b.size == 5759);
  b.clear();
  // Priming is based on decoded duration, not a fixed number of packets.
  assert(b.append(frame, 960, 0));
  assert(!b.ready(10, true));
  assert(b.append(frame, 960, 40));
  assert(!b.ready(40, true));
  assert(b.append(frame, 960, 80));
  assert(b.ready(80, true));
}
