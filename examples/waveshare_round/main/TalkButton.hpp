#pragma once
#include <cstdint>

namespace board {
class TalkButton {
public:
  // Debounce presses; release immediately on an open circuit, including an
  // unplugged switch. A bouncing release must not keep the microphone open.
  bool update(bool pressed, int64_t now_ms) {
    const bool previous = held;
    if (!pressed) {
      candidate_ = held = false;
    } else if (!candidate_) {
      candidate_ = true;
      since_ = now_ms;
    } else if (now_ms - since_ >= 20) {
      held = true;
    }
    return previous != held;
  }
  bool held = false;

private:
  bool candidate_ = false;
  int64_t since_ = 0;
};
} // namespace board
