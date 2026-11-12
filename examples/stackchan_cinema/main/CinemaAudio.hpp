#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace cinema {
// Preserve arbitrary decoder batch sizes and I2S timing while silencing a test.
// A decoded packet can contain substantially more than one 20 ms frame.
template <class Write>
int write_pcm(bool quiet, const int16_t *pcm, size_t samples, Write write) {
  if (!quiet)
    return write(pcm, samples);
  static constexpr int16_t silence[480]{};
  while (samples) {
    const auto count = std::min(samples, size_t(480));
    const int result = write(silence, count);
    if (result)
      return result;
    samples -= count;
  }
  return 0;
}
} // namespace cinema
