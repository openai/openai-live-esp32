#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace live_audio {
// A bounded PCM reserve before starting/restarting hardware playback. Measure
// actual decoded samples: Opus packets need not all contain 20 ms of audio.
class Prebuffer {
public:
  static constexpr size_t RATE = 24000;
  static constexpr unsigned DELAY_MS = 120;
  static constexpr size_t TARGET = RATE * DELAY_MS / 1000;
  static constexpr size_t MAX_PACKET = RATE * 120 / 1000;
  static constexpr size_t CAPACITY = TARGET + MAX_PACKET;
  int16_t pcm[CAPACITY];
  size_t size = 0;

  bool append(const int16_t *input, size_t samples, uint32_t now_ms) {
    if (!input || !samples || samples > MAX_PACKET || size > CAPACITY ||
        samples > CAPACITY - size)
      return false;
    if (!size)
      started_ = now_ms;
    std::memcpy(pcm + size, input, samples * sizeof(int16_t));
    size += samples;
    return true;
  }
  bool ready(uint32_t now_ms, bool enabled) const {
    return size && (!enabled || size >= TARGET ||
                    uint32_t(now_ms - started_) >= DELAY_MS);
  }
  void clear() { size = 0; }

private:
  uint32_t started_ = 0;
};
} // namespace live_audio
