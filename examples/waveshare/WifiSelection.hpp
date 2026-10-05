#pragma once
#include <cstddef>
#include <cstdint>

namespace live_wifi {
constexpr size_t MAX_PROFILES = 8;
// Missing APs use -127. Prefer the strongest eligible saved network, with
// configuration order breaking ties. A failed network cannot starve another.
inline int select(const int *rssi, const int64_t *retry_after, size_t count,
                  int64_t now) {
  int best = -1;
  for (size_t i = 0; i < count && i < MAX_PROFILES; ++i)
    if (rssi[i] > -127 && retry_after[i] <= now &&
        (best < 0 || rssi[i] > rssi[best]))
      best = static_cast<int>(i);
  return best;
}
} // namespace live_wifi
