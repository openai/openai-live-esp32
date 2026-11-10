#include "TranscriptLights.hpp"
#include <cassert>
#include <cstdlib>
#include <set>

using namespace mosaico;
static void smooth(const LightFrame &a, const LightFrame &b) {
  for (unsigned i = 0; i < a.size(); ++i) {
    // Include one unit for rounding the internal fractional values.
    assert(std::abs(int(a[i].r) - b[i].r) <= TranscriptLights::MAX_STEP + 1);
    assert(std::abs(int(a[i].g) - b[i].g) <= TranscriptLights::MAX_STEP + 1);
    assert(std::abs(int(a[i].b) - b[i].b) <= TranscriptLights::MAX_STEP + 1);
  }
}
static void breathing_palette(bool assistant) {
  TranscriptLights lights;
  LightFrame frame{};
  // Check the displayed result, including the board's low-brightness
  // quantization: tiny RGB differences alone aren't visibly different hues.
  auto dimmed = [](LightRgb c) {
    return LightRgb{uint8_t(c.r * 32 / 255), uint8_t(c.g * 32 / 255),
                    uint8_t(c.b * 32 / 255)};
  };
  lights.chunk(assistant, 0);
  int low[6]{255, 255, 255, 255, 255, 255}, high[6]{};
  int distinct_hues = 0, brightness_spread = 0, opposing_breaths = 0;
  LightFrame previous_second{};
  for (int tick = 0; tick < 600; ++tick) {
    const auto previous = frame;
    assert(lights.step(tick * TranscriptLights::FRAME_US,
                       LightMode::active, frame));
    smooth(previous, frame);
    if (tick < 100)
      continue; // Let the initial fade settle; no more chunks are needed.
    std::set<int> hues;
    int darkest = 255, brightest = 0, rising = 0, falling = 0;
    for (unsigned i = 0; i < frame.size(); ++i) {
      const auto c = dimmed(frame[i]);
      const int peak = std::max({c.r, c.g, c.b});
      assert(peak >= 10 && peak <= 28); // Gentle pulses, never dark flashes.
      low[i] = std::min(low[i], peak);
      high[i] = std::max(high[i], peak);
      darkest = std::min(darkest, peak);
      brightest = std::max(brightest, peak);
      if (assistant) {
        assert(c.r > c.g && c.g > c.b);
        hues.insert(10 * c.g / c.r); // Distinguish hue from just brightness.
      } else {
        assert(c.g > c.r + 5 && c.b > c.r + 5);
        hues.insert(10 * c.g / c.b);
      }
      if (tick % 20 == 0) {
        const auto before = dimmed(previous_second[i]);
        const int old_peak = std::max({before.r, before.g, before.b});
        rising += peak > old_peak;
        falling += peak < old_peak;
      }
    }
    distinct_hues += hues.size() >= 3;
    brightness_spread += brightest - darkest >= 3;
    if (tick % 20 == 0) {
      opposing_breaths += rising > 0 && falling > 0;
      previous_second = frame;
    }
  }
  assert(distinct_hues > 450 && brightness_spread > 450);
  assert(opposing_breaths >= 15); // LEDs brighten/dim at different times.
  for (unsigned i = 0; i < frame.size(); ++i)
    assert(high[i] - low[i] >= 4); // Every LED visibly breathes while quiet.
}
int main() {
  breathing_palette(false);
  breathing_palette(true);
  TranscriptLights lights;
  LightFrame frame{};
  int64_t now = 0;
  auto tick = [&](LightMode mode) {
    const auto previous = frame;
    now += TranscriptLights::FRAME_US;
    assert(lights.step(now, mode, frame));
    smooth(previous, frame);
  };
  assert(lights.step(0, LightMode::off, frame));
  assert(frame == LightFrame{});
  for (unsigned i = 0; i < 100; ++i) {
    lights.chunk(false, now);
    tick(LightMode::active);
    const auto previous = frame;
    assert(!lights.step(now + 1000, LightMode::active, frame));
    assert(previous == frame); // Chunk bursts cannot bypass the frame cap.
  }
  for (const auto &c : frame) {
    assert(c.b > c.r + 60 && c.g > c.r + 45);
  }
  const auto cyan = frame;
  bool changed = false;
  for (unsigned i = 0; i < 60; ++i) {
    for (unsigned burst = 0; burst < 1000; ++burst)
      lights.chunk(false, now);
    tick(LightMode::active);
    changed |= frame != cyan;
    for (const auto &c : frame)
      assert(c.b > c.r + 60); // Bounded motion stays in the user's palette.
  }
  assert(changed);
  assert(!(frame.front() == frame.back())); // A gentle spatial color wave.
  lights.chunk(true, now);
  for (unsigned i = 0; i < 100; ++i)
    tick(LightMode::active);
  for (const auto &c : frame)
    assert(c.r > c.g && c.g > c.b && c.r > c.b + 60);
  for (unsigned i = 0; i < 100; ++i) {
    lights.chunk(i % 2, now); // Overlapping speakers still crossfade.
    tick(LightMode::active);
    for (const auto &c : frame)
      assert(int(c.r) + c.g + c.b > 100); // Never blink through black.
  }
  const auto before_stall = frame;
  now += 30000000;
  assert(lights.step(now, LightMode::muted, frame));
  smooth(before_stall, frame); // A slow UI frame cannot jump to the target.
  lights.clear_activity();
  for (unsigned i = 0; i < 120; ++i)
    tick(LightMode::off);
  assert(frame == LightFrame{});
  for (unsigned i = 0; i < 100; ++i)
    tick(LightMode::active);
  for (const auto &c : frame)
    assert(c.b > c.r + 60); // A reconnect starts with no stale assistant turn.
  lights.adopt(-1, {255, 0, 0});
  frame.fill({255, 0, 0});
  lights.hold(now + 10000000);
  now += 10000000;
  tick(LightMode::active); // Explicit colors expire into a smooth transition.
  lights.adopt(2, {0, 255, 0});
  frame[2] = {0, 255, 0};
  tick(LightMode::active);
}
