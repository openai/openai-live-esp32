#include "push_to_talk.hpp"
#include <cassert>

using live_ptt::Frame;

int main() {
  live_ptt::Gate gate;
  assert(gate.frame(0, 0, false, false, 0) == Frame::Skip);
  assert(gate.frame(0, 0, false, true, 0) == Frame::Silence);
  // Speech is captured during connection setup, only while continuously held.
  assert(gate.frame(1, 1, false, false, 0) == Frame::Voice);
  assert(gate.frame(1, 2, false, false, 1) == Frame::Silence);
  assert(gate.frame(1, 3, false, false, 2) == Frame::Silence);
  assert(gate.frame(2, 3, false, false, 3) == Frame::Silence);
  assert(gate.frame(3, 3, false, false, 4) == Frame::Voice);
  assert(gate.frame(3, 3, true, true, 5) == Frame::Silence);
  // Release before connection retains the utterance and its silence tail.
  // Further unheld audio is neither recorded nor allowed to fill the queue.
  for (unsigned i = 1; i < 600 / live_ptt::FRAME_MS; ++i)
    assert(gate.frame(4, 4, false, false, 30) == Frame::Silence);
  assert(gate.frame(4, 4, false, false, 30) == Frame::Skip);
  assert(gate.frame(4, 4, false, true, 30) == Frame::Skip);
  assert(gate.frame(4, 4, false, true, 0) == Frame::Silence);
  assert(gate.frame(5, 5, false, true, 0) == Frame::Voice);
  // A new session has no retained gate state or release tail.
  live_ptt::Gate restarted;
  assert(restarted.frame(6, 6, false, false, 0) == Frame::Skip);

  live_ptt::Pacer pace;
  assert(pace.due(1000000));
  pace.sent(1000000);
  assert(!pace.due(1000000) && !pace.due(1019999));
  assert(pace.due(1020000));
  pace.sent(1020000);
  // A multi-second network/CPU stall must not dump the buffered utterance.
  assert(pace.due(5000000));
  pace.sent(5000000);
  assert(!pace.due(5000000) && pace.due(5020000));

  // Simulate release during a two-second startup, then replay all buffered
  // speech and silence at exactly 50 packets/second without recording idle mic.
  live_ptt::Gate startup;
  unsigned queued = 0, voice = 0;
  for (unsigned i = 0; i < 100; ++i) {
    auto frame = startup.frame(i < 25 ? 1 : 2, i < 25 ? 1 : 2,
                               false, false, queued);
    voice += frame == Frame::Voice;
    queued += frame != Frame::Skip;
  }
  assert(voice == 25 && queued == 55);
  live_ptt::Pacer replay;
  unsigned delivered = 0;
  for (int64_t us = 2000000; us < 3100000; us += 1000) {
    if (queued && replay.due(us)) {
      --queued;
      ++delivered;
      replay.sent(us);
    }
  }
  assert(delivered == 55 && queued == 0);
}
