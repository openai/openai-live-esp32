#include "MotionPolicy.hpp"
#include "client_tools.hpp"
#include <cassert>
#include <string>
#include <vector>
using namespace stackchan;
int main() {
  Pose p{9, 25};
  assert(parse_pose("{\"yaw_degrees\":-40,\"pitch_degrees\":60}", p));
  assert(p.yaw == -40 && p.pitch == 60);
  for (const char *bad :
       {"{}", "[]", "null", "{\"yaw_degrees\":0}",
        "{\"yaw_degrees\":41,\"pitch_degrees\":15}",
        "{\"yaw_degrees\":0,\"pitch_degrees\":9}",
        "{\"yaw_degrees\":0,\"pitch_degrees\":61}",
        "{\"yaw_degrees\":0.5,\"pitch_degrees\":15}",
        "{\"yaw_degrees\":1e99,\"pitch_degrees\":15}",
        "{\"yaw_degrees\":\"0\",\"pitch_degrees\":15}",
        "{\"yaw_degrees\":0,\"pitch_degrees\":15,\"yaw_degrees\":10}",
        "{\"yaw_degrees\":0,\"pitch_degrees\":15,\"speed\":1000}",
        "{\"yaw_degrees\\u0000x\":0,\"pitch_degrees\":15}",
        "{\"yaw_degrees\":0,\"pitch_degrees\":15} trailing"}) {
    const auto before = p;
    assert(!parse_pose(bad, p));
    assert(p.yaw == before.yaw && p.pitch == before.pitch);
  }
  assert(calibration_valid(YAW_ZERO, PITCH_ZERO));
  for (auto zeros :
       {Pose{-1, 620}, Pose{1001, 620}, Pose{460, 900}, Pose{100, 620}})
    assert(!calibration_valid(zeros.yaw, zeros.pitch));
  assert(raw(0, 460) == 460 && raw(15, 620) == 668);
  assert(degrees(668, 620) == 15.0);
  for (int start : {raw(-40, YAW_ZERO), YAW_ZERO, raw(40, YAW_ZERO)}) {
    for (int end : {raw(-40, YAW_ZERO), YAW_ZERO, raw(40, YAW_ZERO)}) {
      const unsigned duration = duration_ms(end - start, 0);
      assert(duration >= 400 && duration <= 3500);
      int prev = start;
      for (unsigned t = 0; t <= duration + 20; t += 20) {
        int point = interpolate(start, end, t, duration);
        assert(point >= std::min(start, end) && point <= std::max(start, end));
        assert(std::abs(point - prev) <=
               4); // <=1.25 deg per 20 ms, rounding included.
        if (end >= start)
          assert(point >= prev);
        else
          assert(point <= prev);
        prev = point;
      }
      assert(interpolate(start, end, duration, duration) == end);
    }
  }
  uint8_t out[16], params[]{56, 2};
  assert(packet(1, 2, params, 2, out, sizeof(out)) == 8);
  const uint8_t expected[]{255, 255, 1, 4, 2, 56, 2, 190};
  assert(!memcmp(out, expected, sizeof(expected)));
  assert(!packet(0, 2, params, 2, out, sizeof(out)));
  assert(!packet(254, 2, params, 2, out, sizeof(out)));
  assert(!packet(1, 2, params, 8, out, sizeof(out)));
  assert(!packet(1, 2, params, 2, out, 7));
  uint8_t reply[]{255, 255, 1, 4, 0, 1, 204, 45}; // position 460, checksum
  assert(response(reply, sizeof(reply), 1, 2));
  assert(!response(reply, sizeof(reply) - 1, 1, 2));
  assert(!response(reply, sizeof(reply), 2, 2));
  for (size_t i = 0; i < sizeof(reply); ++i) {
    reply[i] ^= 1;
    assert(!response(reply, sizeof(reply), 1, 2));
    reply[i] ^= 1;
  }
  reply[4] = 4;
  reply[7] -= 4; // Valid checksum, servo overload status is still an error.
  assert(!response(reply, sizeof(reply), 1, 2));
  std::string wire;
  assert(live_tools::configure(TOOLS_JSON, true, wire));
  assert(live_tools::declared(wire, "look_at"));
  assert(live_tools::declared(wire, "capture_photo"));
  assert(!live_tools::declared(wire, "rotate_forever"));
  assert(wire.size() < 4096);
  assert(live_tools::configure(nullptr, true, wire));
  assert(wire == live_photo::TOOL_JSON);
  assert(live_tools::configure(nullptr, false, wire) && wire.empty());
  for (auto *bad :
       {"[]", "{}", "null", "[{}]", "[] trailing", live_photo::TOOL_JSON,
        "[{\"type\":\"function\",\"name\":\"x\",\"parameters\":{}},{\"type\":"
        "\"function\",\"name\":\"x\",\"parameters\":{}}]"})
    assert(!live_tools::configure(bad, true, wire));
  live_photo::Batch batch;
  assert(batch.set_delegation("d1") && batch.begin("r1"));
  assert(batch.add("c1", nullptr, "look_at",
                   "{\"yaw_degrees\":10,\"pitch_degrees\":15}") ==
         live_photo::Added::accepted);
  assert(!strcmp(batch.next()->name, "look_at"));
  assert(batch.add("c1", nullptr, "center_head", "{}") ==
         live_photo::Added::duplicate);
  assert(batch.add("c2", nullptr, "look_at", std::string(513, 'x').c_str()) ==
         live_photo::Added::invalid);
  const auto generation = batch.generation;
  batch.cancel();
  assert(batch.generation != generation && !batch.next());
  assert(batch.begin("r1") &&
         !batch.next()); // Do not restart a cancelled response.
  assert(batch.set_delegation("d2") && batch.begin("r2"));
  assert(batch.add("c1", nullptr, "center_head", "{}") ==
         live_photo::Added::accepted);
  assert(batch.complete("r2") && !batch.ready());
  assert(batch.submitted("c1") && batch.ready());
}
