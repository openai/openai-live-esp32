#pragma once
#include "UsageSummary.hpp"
#include "live_transcript.h"
#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {
class LiveVoice final : public systems::phone::App {
public:
  static LiveVoice *requestInstance();
  static void provisionWifi();
  void inspectScreen(bool capture, bool reset = false);

protected:
  LiveVoice();
  bool init() override;
  bool deinit() override;
  bool run() override;
  bool back() override;
  bool close() override;
  bool pause() override;
  bool resume() override;

private:
  void refresh();
  lv_obj_t *title_ = nullptr, *status_ = nullptr;
  lv_obj_t *usage_ = nullptr;
  lv_obj_t *usage_values_[live_usage_view::ROW_COUNT]{};
  lv_obj_t *usage_note_ = nullptr;
  lv_obj_t *transcripts_[LIVE_TRANSCRIPT_BLOCKS]{};
  lv_obj_t *transcript_area_ = nullptr;
  uint32_t transcript_revision_ = UINT32_MAX;
  live_transcript_t transcript_snapshot_{};
  lv_obj_t *control_ = nullptr, *control_text_ = nullptr;
  int control_mode_ = -1;
  lv_timer_t *timer_ = nullptr;
};
} // namespace esp_brookesia::apps
