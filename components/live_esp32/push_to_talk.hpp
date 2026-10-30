#pragma once
#include <cstdint>

namespace live_ptt {
constexpr unsigned FRAME_MS = 20;
constexpr unsigned BUFFER_FRAMES = 10000 / FRAME_MS;

enum class Frame { Skip, Silence, Voice };

// Called only by the capture task. A frame crossing any button transition
// must be silent: an epoch, rather than just a boolean, catches a release and
// repress during the same microphone read. Preserve a short silence between
// utterances, then let queued startup speech drain instead of buffering an
// unlimited amount of silence while the user waits for the connection.
class Gate {
public:
  Frame frame(uint32_t before, uint32_t after, bool muted, bool connected,
              unsigned queued) {
    if (!muted && (before & 1u) && before == after) {
      tail_ = 600 / FRAME_MS;
      return Frame::Voice;
    }
    if (tail_) {
      --tail_;
      return Frame::Silence;
    }
    return connected && queued == 0 ? Frame::Silence : Frame::Skip;
  }

private:
  unsigned tail_ = 0;
};

// Only the transport owner calls this. Replay startup speech at its recorded
// rate, not as a burst that would overrun the receiver's jitter buffer. A long
// scheduling stall rebases the clock rather than releasing a catch-up burst.
class Pacer {
public:
  bool due(int64_t now_us) const { return !next_us_ || now_us >= next_us_; }
  void sent(int64_t now_us) {
    constexpr int64_t period = FRAME_MS * 1000;
    if (!next_us_ || now_us - next_us_ >= period)
      next_us_ = now_us;
    next_us_ += period;
  }

private:
  int64_t next_us_ = 0;
};
} // namespace live_ptt
