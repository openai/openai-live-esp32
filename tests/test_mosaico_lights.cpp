#include "TranscriptLights.hpp"
#include <cassert>
#include <cstdlib>

using namespace mosaico;
static void smooth(const LightFrame &a, const LightFrame &b) {
  for (unsigned i = 0; i < a.size(); ++i) {
    // Include one unit for rounding the internal fractional values.
    assert(std::abs(int(a[i].r) - b[i].r) <= TranscriptLights::MAX_STEP + 1);
    assert(std::abs(int(a[i].g) - b[i].g) <= TranscriptLights::MAX_STEP + 1);
    assert(std::abs(int(a[i].b) - b[i].b) <= TranscriptLights::MAX_STEP + 1);
  }
}
int main() {
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
    assert(c.b > c.r + 80 && c.g > c.r + 60);
  }
  const auto cyan = frame;
  bool changed = false;
  for (unsigned i = 0; i < 60; ++i) {
    for (unsigned burst = 0; burst < 1000; ++burst)
      lights.chunk(false, now);
    tick(LightMode::active);
    changed |= frame != cyan;
    for (const auto &c : frame)
      assert(c.b > c.r + 80); // Bounded motion stays in the user's palette.
  }
  assert(changed);
  assert(!(frame.front() == frame.back())); // A gentle spatial color wave.
  lights.chunk(true, now);
  for (unsigned i = 0; i < 100; ++i)
    tick(LightMode::active);
  for (const auto &c : frame)
    assert(c.r > c.g && c.g > c.b && c.r > c.b + 80);
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
    assert(c.b > c.r + 80); // A reconnect starts with no stale assistant turn.
  lights.adopt(-1, {255, 0, 0});
  frame.fill({255, 0, 0});
  lights.hold(now + 10000000);
  now += 10000000;
  tick(LightMode::active); // Explicit colors expire into a smooth transition.
  lights.adopt(2, {0, 255, 0});
  frame[2] = {0, 255, 0};
  tick(LightMode::active);
}
