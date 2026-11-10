#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace mosaico {
struct LightRgb {
  uint8_t r = 0, g = 0, b = 0;
  bool operator==(const LightRgb &other) const {
    return r == other.r && g == other.g && b == other.b;
  }
};
using LightFrame = std::array<LightRgb, 6>;
enum class LightMode { off, connecting, active, muted };

// Allocation-free, time-driven animation. Staggered hue and breathing waves
// keep the LEDs distinct even through quiet passages. Chunks nudge a bounded
// phase target, never restart a fade or write hardware from the peer/audio tasks.
class TranscriptLights {
  float current_[6][3]{};
  float phase_ = 0, breath_phase_ = 0, pending_motion_ = 0;
  int64_t last_step_ = -1, last_chunk_ = -1;
  bool assistant_ = false;

public:
  static constexpr int64_t FRAME_US = 50000; // At most 20 full-strip writes/s.
  static constexpr float MAX_STEP = 10;      // Before the BSP's 32/255 dimmer.

  void clear_activity() {
    last_chunk_ = -1;
    assistant_ = false;
    pending_motion_ = 0;
  }
  void chunk(bool assistant, int64_t now) {
    assistant_ = assistant;
    last_chunk_ = now;
    pending_motion_ = std::min(0.8f, pending_motion_ + 0.18f);
  }
  // Adopt explicit tool colors so their expiry blends back from what is
  // actually visible, including individual-LED overrides.
  void adopt(int index, LightRgb color) {
    for (int i = 0; i < 6; ++i) {
      if (index >= 0 && i != index)
        continue;
      current_[i][0] = color.r;
      current_[i][1] = color.g;
      current_[i][2] = color.b;
    }
  }
  void hold(int64_t now) { last_step_ = now; }
  bool step(int64_t now, LightMode mode, LightFrame &frame) {
    if (last_step_ >= 0 && now - last_step_ < FRAME_US)
      return false;
    const float dt = last_step_ < 0
                         ? 0.05f
                         : std::min(0.1f, float(now - last_step_) / 1000000);
    last_step_ = now;
    const float activity =
        last_chunk_ < 0
            ? 0
            : std::clamp(1.f - float(now - last_chunk_) / 3000000.f, 0.f, 1.f);
    if (mode == LightMode::active) {
      const float advance = std::min(pending_motion_, dt * 0.65f);
      // Slow continuous drift plus chunk-driven motion. The two periods are
      // deliberately different so color and brightness don't march together.
      phase_ += dt * 0.32f + advance;
      breath_phase_ += dt * (0.85f + 0.12f * activity);
      if (phase_ >= 6.2831853f)
        phase_ -= 6.2831853f;
      if (breath_phase_ >= 6.2831853f)
        breath_phase_ -= 6.2831853f;
      pending_motion_ -= advance;
    }
    const float alpha = dt / (0.55f + dt);
    for (unsigned i = 0; i < frame.size(); ++i) {
      float target[3]{};
      if (mode == LightMode::connecting) {
        target[0] = 120;
        target[1] = 65;
        target[2] = 12;
      } else if (mode == LightMode::muted) {
        target[0] = 150;
        target[1] = 20;
        target[2] = 35;
      } else if (mode == LightMode::active) {
        const float wave = std::sin(phase_ + float(i) * 1.0471976f);
        const float breath = std::sin(breath_phase_ - float(i) * 1.7f);
        const float gain = 0.61f + 0.08f * activity +
                           (0.13f + 0.04f * activity) * breath;
        // Teal/cyan/blue or amber/gold, with enough separation to survive
        // the BSP's 32/255 dimmer. Breaths stay lit at their troughs.
        target[0] = (assistant_ ? 240.f + 10 * wave : 26.f + 12 * wave) * gain;
        target[1] = (assistant_ ? 148.f + 48 * wave : 184.f + 55 * wave) * gain;
        target[2] = (assistant_ ? 42.f + 22 * wave : 230.f - 25 * wave) * gain;
      }
      uint8_t channels[3];
      for (unsigned c = 0; c < 3; ++c) {
        auto &value = current_[i][c];
        value += std::clamp((target[c] - value) * alpha, -MAX_STEP, MAX_STEP);
        channels[c] = uint8_t(std::clamp(std::lround(value), 0L, 255L));
      }
      frame[i] = {channels[0], channels[1], channels[2]};
    }
    return true;
  }
};
} // namespace mosaico
