#include "LightsPolicy.hpp"
#include <cassert>
#include <set>
using namespace stackchan::lights;

static unsigned color(const Packet &packet, unsigned i) {
  return packet[1 + 2 * i] | unsigned(packet[2 + 2 * i]) << 8;
}
static std::array<int, 3> rgb(const Packet &p, unsigned i) {
  const auto c = color(p, i);
  return {int((c >> 11) & 31) * 8, int((c >> 5) & 63) * 4, int(c & 31) * 8};
}
int main() {
  Frame frame{};
  auto off = encode(frame);
  assert(off.size() == 25 && off[0] == 0x30);
  assert(CONFIG == 0x24 && REFRESH == 0x4c);
  for (unsigned i = 0; i < COUNT; ++i)
    assert(!color(off, i));
  frame[0] = {255, 255, 255};
  frame[11] = {0, 0, 255};
  const auto bytes = encode(frame);
  assert(bytes[1] == 0x08 &&
         bytes[2] == 0x42); // Dimmed RGB565, low byte first.
  assert(bytes[23] == 0x08 && bytes[24] == 0);
  for (unsigned i = 1; i < 11; ++i)
    assert(!color(bytes, i));
  for (bool assistant : {false, true}) {
    Animation flow;
    Frame frame{};
    Packet previous = off;
    int low[COUNT], high[COUNT]{};
    std::fill(std::begin(low), std::end(low), 255);
    unsigned distinct = 0;
    for (unsigned tick = 0; tick < 600; ++tick) {
      const int64_t now = tick * Animation::FRAME_US;
      if (tick % 5 == 0)
        flow.chunk(assistant, now);
      assert(flow.step(now, Mode::active, frame));
      assert(!flow.step(now + 1000, Mode::active, frame));
      auto packet = encode(frame);
      std::set<unsigned> colors;
      for (unsigned i = 0; i < COUNT; ++i) {
        const auto value = rgb(packet, i), old = rgb(previous, i);
        for (unsigned c = 0; c < 3; ++c) {
          assert(value[c] <= 64);
          assert(std::abs(value[c] - old[c]) <= 8); // Quantized, gentle steps.
        }
        if (tick > 100) {
          const auto peak = std::max({value[0], value[1], value[2]});
          assert(peak >= 16); // Pulses do not switch off between chunks.
          low[i] = std::min(low[i], peak);
          high[i] = std::max(high[i], peak);
          if (assistant)
            assert(value[0] >= value[1] && value[1] > value[2]);
          else
            assert(value[1] > value[0] + 8 && value[2] > value[0] + 8);
          colors.insert(color(packet, i));
        }
      }
      distinct += colors.size() >= 4;
      previous = packet;
    }
    assert(distinct > 450);
    for (unsigned i = 0; i < COUNT; ++i)
      assert(high[i] - low[i] >= 8); // Both six-LED banks actually breathe.
    flow.clear_activity();
    for (unsigned tick = 600; tick < 900; ++tick)
      flow.step(tick * Animation::FRAME_US, Mode::off, frame);
    assert(encode(frame) == off);
  }
}
