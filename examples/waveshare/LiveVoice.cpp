#include "LiveVoice.hpp"
#include "ScreenMirror.hpp"
#include "UsageSummary.hpp"
#include "WifiProfiles.hpp"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/idf_additions.h"
#include "live_esp32.h"
#include "live_voice_secrets.h"
#include "system_status.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
bool owns_audio = false;
esp_err_t audio_open(void *) {
  esp_err_t e = bsp_extra_audio_session_acquire(BSP_EXTRA_AUDIO_OWNER_LIVE);
  if (e != ESP_OK)
    return e;
  owns_audio = true;
  e = bsp_extra_codec_set_voice_fs(24000, 16);
  if (e == ESP_OK)
    e = bsp_extra_codec_volume_set(100, nullptr);
  return e;
}
void audio_close(void *) {
  if (!owns_audio)
    return;
  bsp_extra_codec_dev_stop();
  bsp_extra_audio_session_release(BSP_EXTRA_AUDIO_OWNER_LIVE);
  owns_audio = false;
}
esp_err_t read_pcm(void *, int16_t *pcm, size_t samples) {
  size_t n = 0;
  esp_err_t e = bsp_extra_i2s_read(pcm, samples * sizeof(*pcm), &n, 1000);
  return e == ESP_OK && n != samples * sizeof(*pcm) ? ESP_FAIL : e;
}
esp_err_t write_pcm(void *, const int16_t *pcm, size_t samples) {
  size_t n = 0;
  esp_err_t e = bsp_extra_i2s_write(const_cast<int16_t *>(pcm),
                                    samples * sizeof(*pcm), &n, 1000);
  return e == ESP_OK && n != samples * sizeof(*pcm) ? ESP_FAIL : e;
}
void start() {
  if (!live_wifi::ready()) {
    ESP_LOGW("LiveVoice", "Wait for Wi-Fi and DHCP before starting");
    return;
  }
  // Wi-Fi power saving otherwise adds avoidable latency to voice packets.
  esp_wifi_set_ps(WIFI_PS_NONE);
  ESP_LOGI("LiveVoice", "Start requested: %s",
           esp_err_to_name(live_esp32_start()));
}
void wifi_status() {
  live_wifi::print_status();
  brookesia::system_status::Snapshot s{};
  brookesia::system_status::get_snapshot(s);
  esp_netif_ip_info_t ip{};
  auto *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif)
    esp_netif_get_ip_info(netif, &ip);
  ESP_LOGI("LiveVoice", "wifi enabled=%d connected=%d rssi=%d ip=" IPSTR,
           s.wifi_enabled, s.wifi_connected, s.wifi_rssi, IP2STR(&ip.ip));
  ESP_LOGI("LiveVoice",
           "power pmu=%d battery_valid=%d battery_present=%d "
           "percent=%d charging=%d charger_status=%u",
           s.pmu_present, s.battery_valid, s.battery_present, s.battery_percent,
           s.charging, s.charger_status);
}
void wifi_scan() {
  live_esp32_status_t s{};
  live_esp32_status(&s);
  if (s.busy) {
    ESP_LOGW("LiveVoice", "End the Live session before scanning Wi-Fi");
    return;
  }
  uint16_t count = 64;
  auto *records =
      static_cast<wifi_ap_record_t *>(calloc(count, sizeof(wifi_ap_record_t)));
  if (!records) {
    ESP_LOGW("LiveVoice", "Wi-Fi scan: insufficient memory");
    return;
  }
  // The resident manager pauses reconnects until the scan results are copied.
  esp_err_t e = brookesia::system_status::scan_wifi(records, &count);
  if (e != ESP_OK)
    count = 0;
  ESP_LOGI("LiveVoice", "Wi-Fi scan: %s, %u access points (limit 64)",
           esp_err_to_name(e), count);
  for (unsigned i = 0; e == ESP_OK && i < count; ++i) {
    const auto &ap = records[i];
    ESP_LOGI("LiveVoice", "AP ssid=\"%.32s\" channel=%u rssi=%d auth=%d",
             reinterpret_cast<const char *>(ap.ssid), ap.primary, ap.rssi,
             static_cast<int>(ap.authmode));
  }
  free(records);
}
void console(void *) {
  // Small, credential-free USB control surface for repeatable hardware smoke
  // tests.
  char line[40];
  size_t n = 0;
  for (;;) {
    int c = getchar();
    if (c == EOF) {
      clearerr(stdin);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (c == '\r' || c == '\n') {
      line[n] = 0;
      n = 0;
      if (!strcmp(line, "live start"))
        start();
      else if (!strcmp(line, "live audio-test"))
        live_esp32_audio_test();
      else if (!strcmp(line, "live peer-test"))
        ESP_LOGI("LiveVoice", "Peer test requested: %s",
                 esp_err_to_name(live_esp32_peer_test()));
      else if (!strcmp(line, "live stop"))
        live_esp32_stop();
      else if (!strcmp(line, "live mute"))
        live_esp32_mute(true);
      else if (!strcmp(line, "live unmute"))
        live_esp32_mute(false);
      else if (!strcmp(line, "live wifi-next"))
        live_wifi::next();
      else if (!strcmp(line, "live timing"))
        live_esp32_log_startup();
      else if (!strcmp(line, "live diagnostics"))
        live_esp32_log_diagnostics();
      else if (!strcmp(line, "live mirror-on"))
        live_mirror::enable(true);
      else if (!strcmp(line, "live mirror-off"))
        live_mirror::enable(false);
      else if (!strcmp(line, "live mirror-key"))
        live_mirror::keyframe();
      else if (!strcmp(line, "live mirror-status"))
        live_mirror::status();
      else if (!strcmp(line, "live preconnect-on"))
        live_esp32_preconnect(true);
      else if (!strcmp(line, "live preconnect-off"))
        live_esp32_preconnect(false);
      else if (!strcmp(line, "live screen"))
        esp_brookesia::apps::LiveVoice::requestInstance()->inspectScreen(false);
      else if (!strcmp(line, "live screenshot"))
        esp_brookesia::apps::LiveVoice::requestInstance()->inspectScreen(true);
      else if (!strcmp(line, "live panel-reset"))
        esp_brookesia::apps::LiveVoice::requestInstance()->inspectScreen(false,
                                                                         true);
      else if (!strcmp(line, "live wifi-scan"))
        wifi_scan();
      else if (!strcmp(line, "live wifi-reconnect")) {
        ESP_LOGI(
            "LiveVoice", "Wi-Fi reconnect: %s",
            esp_err_to_name(brookesia::system_status::request_wifi_connect()));
      } else if (!strcmp(line, "live status")) {
        live_esp32_status_t s{};
        live_esp32_status(&s);
        ESP_LOGI("LiveVoice",
                 "status=%s active=%d busy=%d finalized=%d tx=%lu rx=%lu "
                 "played=%lu id=%s",
                 s.status, s.active, s.busy, s.finalized,
                 (unsigned long)s.sent_packets,
                 (unsigned long)s.received_packets,
                 (unsigned long)s.played_samples, s.session_id);
        wifi_status();
      }
    } else if (n + 1 < sizeof(line))
      line[n++] = c;
    else
      n = 0;
  }
}
lv_obj_t *button(lv_obj_t *parent, const char *text, int width, uint32_t color,
                 lv_event_cb_t cb, void *ctx) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, width, 72);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x3D5360), LV_STATE_DISABLED);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_DISABLED);
  lv_obj_set_style_opa(b, LV_OPA_COVER, LV_STATE_DISABLED);
  lv_obj_set_style_radius(b, 18, 0);
  lv_obj_t *label = lv_label_create(b);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_22, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ctx);
  return b;
}
} // namespace
namespace esp_brookesia::apps {
LiveVoice *LiveVoice::requestInstance() {
  static auto *instance = new LiveVoice();
  return instance;
}
LiveVoice::LiveVoice() : App("Live Voice", nullptr, true, true, true) {}
bool LiveVoice::init() {
  live_esp32_config_t cfg{};
  cfg.api_key = LIVE_API_KEY;
  cfg.model = LIVE_MODEL;
  cfg.responses_model = LIVE_RESPONSES_MODEL;
  cfg.responses_service_tier = "priority";
  cfg.responses_web_search = true;
  cfg.responses_instructions =
      "Use web search for current facts and internet lookup requests. Return "
      "concise, factual findings with source URLs for the voice assistant. "
      "Say when information could not be verified.";
  cfg.audio_open = audio_open;
  cfg.audio_close = audio_close;
  cfg.read = read_pcm;
  cfg.write = write_pcm;
  if (live_esp32_init(&cfg) != ESP_OK)
    return false;
  if (!live_mirror::init())
    ESP_LOGW("LiveVoice", "USB screen mirror unavailable");
  return xTaskCreatePinnedToCoreWithCaps(
             console, "live_usb", 8192, nullptr, 2, nullptr, 1,
             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}
void LiveVoice::provisionWifi() { live_wifi::start(); }
bool LiveVoice::run() {
  live_esp32_preconnect(true);
  lv_obj_t *screen = lv_screen_active();
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x0A1721), 0);
  lv_obj_set_style_text_color(screen, lv_color_hex(0xF5FAFC), 0);
  lv_obj_set_style_pad_all(screen, 0, 0);
  auto area = getVisualArea();
  const int w = lv_area_get_width(&area), h = lv_area_get_height(&area);
  // Brookesia already positions the app screen within its visual area. Inset
  // its contents again for the physical AMOLED corners and navigation gesture.
  constexpr int inset = 28;
  lv_obj_t *content = lv_obj_create(screen);
  lv_obj_remove_style_all(content);
  lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(content, inset, inset);
  lv_obj_set_size(content, w - 2 * inset, h - 2 * inset);
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(content, 8, 0);
  title_ = lv_label_create(content);
  lv_label_set_text(title_, "Live Voice");
  lv_obj_set_style_text_font(title_, &lv_font_montserrat_28, 0);
  lv_obj_set_width(title_, LV_PCT(100));
  lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_CENTER, 0);
  status_ = lv_label_create(content);
  lv_obj_set_size(status_, LV_PCT(100), 44);
  lv_label_set_long_mode(status_, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(status_, &lv_font_montserrat_18, 0);
  usage_ = lv_obj_create(content);
  lv_obj_remove_style_all(usage_);
  lv_obj_set_size(usage_, LV_PCT(100), 0);
  lv_obj_set_flex_grow(usage_, 1);
  lv_obj_set_scroll_dir(usage_, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(usage_, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(usage_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(usage_, 6, 0);
  for (unsigned i = 0; i < live_usage_view::ROW_COUNT; ++i) {
    auto *row = lv_obj_create(usage_);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    auto *key = lv_label_create(row);
    lv_obj_set_width(key, 96);
    lv_label_set_text(key, live_usage_view::KEYS[i]);
    lv_obj_set_style_text_font(key, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(key, lv_color_hex(0x84DBFF), 0);
    auto *value = usage_values_[i] = lv_label_create(row);
    lv_obj_set_width(value, 0);
    lv_obj_set_flex_grow(value, 1);
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(value, lv_color_hex(0xF5FAFC), 0);
  }
  usage_note_ = lv_label_create(usage_);
  lv_obj_set_width(usage_note_, LV_PCT(100));
  lv_label_set_text(usage_note_, "Partial token totals");
  lv_obj_set_style_text_font(usage_note_, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(usage_note_, lv_color_hex(0xFFD294), 0);
  lv_obj_add_flag(usage_note_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(usage_, LV_OBJ_FLAG_HIDDEN);
  transcript_area_ = lv_obj_create(content);
  lv_obj_remove_style_all(transcript_area_);
  lv_obj_set_size(transcript_area_, LV_PCT(100), 0);
  lv_obj_set_flex_grow(transcript_area_, 1);
  lv_obj_set_scroll_dir(transcript_area_, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(transcript_area_, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(transcript_area_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(transcript_area_, 12, 0);
  for (auto &label : transcripts_) {
    label = lv_label_create(transcript_area_);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_text(label, "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_22, 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
  }
  transcript_revision_ = UINT32_MAX;
  control_ = button(
      content, "Start", LV_PCT(100), 0x137A55,
      [](lv_event_t *event) {
        live_esp32_status_t s{};
        live_esp32_status(&s);
        ESP_LOGI("LiveVoice", "Touch control: %s", s.busy ? "Stop" : "Start");
        if (s.busy)
          live_esp32_stop();
        else
          start();
        static_cast<LiveVoice *>(lv_event_get_user_data(event))->refresh();
      },
      this);
  control_text_ = lv_obj_get_child(control_, 0);
  control_mode_ = -1;
  timer_ = lv_timer_create(
      [](lv_timer_t *timer) {
        static_cast<LiveVoice *>(lv_timer_get_user_data(timer))->refresh();
      },
      200, this);
  refresh();
  return timer_ != nullptr;
}
void LiveVoice::refresh() {
  live_esp32_status_t s{};
  live_esp32_status(&s);
  const bool network_ready = live_wifi::ready();
  const bool connected = s.active && network_ready;
  const bool show_usage = !s.busy && s.usage.started;
  if (connected) {
    lv_obj_add_flag(title_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(title_, LV_OBJ_FLAG_HIDDEN);
    if (show_usage && s.usage.seconds_final)
      lv_obj_add_flag(status_, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_remove_flag(status_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(status_, show_usage ? 22 : 44);
    const char *text = show_usage
                           ? "Last reported"
                           : (network_ready ? s.status : "Connecting Wi-Fi...");
    if (strcmp(lv_label_get_text(status_), text))
      lv_label_set_text(status_, text);
  }
  const char *title = show_usage ? "Session usage" : "Live Voice";
  if (strcmp(lv_label_get_text(title_), title))
    lv_label_set_text(title_, title);
  if (show_usage) {
    for (unsigned i = 0; i < live_usage_view::ROW_COUNT; ++i) {
      char text[128];
      live_usage_view::format(s.usage, static_cast<live_usage_view::Row>(i),
                              text, sizeof(text));
      if (strcmp(lv_label_get_text(usage_values_[i]), text))
        lv_label_set_text(usage_values_[i], text);
    }
    if (s.usage.tokens_partial)
      lv_obj_remove_flag(usage_note_, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(usage_note_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(usage_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(transcript_area_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(usage_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(transcript_area_, LV_OBJ_FLAG_HIDDEN);
  }
  // Update labels only when their contents change; packet counts stay on USB.
  if (transcript_revision_ != s.transcript_revision) {
    live_esp32_transcripts(&transcript_snapshot_);
    for (unsigned i = 0; i < LIVE_TRANSCRIPT_BLOCKS; ++i) {
      auto *label = transcripts_[i];
      if (i >= transcript_snapshot_.count) {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      const auto &block = transcript_snapshot_.blocks[i];
      const bool user = block.speaker == LIVE_SPEAKER_USER;
      if (strcmp(lv_label_get_text(label), block.text))
        lv_label_set_text(label, block.text);
      lv_obj_set_style_text_color(label,
                                  lv_color_hex(user ? 0x84DBFF : 0xFFD294), 0);
      lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
    transcript_revision_ = transcript_snapshot_.revision;
    lv_obj_update_layout(transcript_area_);
    lv_obj_scroll_to_y(transcript_area_, LV_COORD_MAX, LV_ANIM_OFF);
  }
  const int mode = s.busy ? (connected ? 2 : 1) : (network_ready ? 0 : 3);
  if (mode != control_mode_) {
    const uint32_t colors[] = {0x137A55, 0xF2AF38, 0xC63C49, 0x3D5360};
    lv_label_set_text(control_text_, s.busy ? "Stop" : "Start");
    lv_obj_set_style_bg_color(control_, lv_color_hex(colors[mode]), 0);
    lv_obj_set_style_text_color(
        control_text_, lv_color_hex(mode == 1 ? 0x0A1721 : 0xFFFFFF), 0);
    if (mode == 3)
      lv_obj_add_state(control_, LV_STATE_DISABLED);
    else
      lv_obj_remove_state(control_, LV_STATE_DISABLED);
    ESP_LOGI("LiveVoice", "UI control=%s connected=%d enabled=%d",
             s.busy ? "Stop" : "Start", connected, mode != 3);
    control_mode_ = mode;
  }
}
void LiveVoice::inspectScreen(bool capture, bool reset) {
  if (!bsp_display_lock(1000)) {
    ESP_LOGW("LiveVoice", "Display lock timed out");
    return;
  }
  const esp_err_t result =
      reset ? bsp_display_recover() : bsp_display_brightness_set(70);
  ESP_LOGI("LiveVoice", "Panel %s: %s", reset ? "reset" : "brightness",
           esp_err_to_name(result));
  lv_obj_t *screen = lv_screen_active();
  lv_obj_update_layout(screen);
  lv_obj_invalidate(screen);
  lv_area_t bounds{};
  lv_obj_get_coords(screen, &bounds);
  ESP_LOGI("LiveVoice", "Screen bounds=%ld,%ld..%ld,%ld", (long)bounds.x1,
           (long)bounds.y1, (long)bounds.x2, (long)bounds.y2);
  if (control_) {
    lv_obj_get_coords(control_, &bounds);
    ESP_LOGI("LiveVoice", "Control visible=%d bounds=%ld,%ld..%ld,%ld mode=%d",
             lv_obj_is_visible(control_), (long)bounds.x1, (long)bounds.y1,
             (long)bounds.x2, (long)bounds.y2, control_mode_);
  }
  std::vector<uint8_t> pixels;
  uint32_t width = 0, height = 0, stride = 0;
  if (capture) {
    lv_draw_buf_t *snapshot = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    if (snapshot) {
      width = snapshot->header.w;
      height = snapshot->header.h;
      stride = snapshot->header.stride;
      pixels.assign(snapshot->data, snapshot->data + stride * height);
      lv_draw_buf_destroy(snapshot);
    } else {
      ESP_LOGE("LiveVoice", "Screen snapshot allocation failed");
    }
  }
  bsp_display_unlock();
  if (!pixels.empty()) {
    printf("LVGL_RGB565 %lu %lu %lu\n", (unsigned long)width,
           (unsigned long)height, (unsigned long)stride);
    constexpr char hex[] = "0123456789abcdef";
    for (size_t offset = 0; offset < pixels.size(); offset += 128) {
      char line[257];
      size_t count = std::min(size_t(128), pixels.size() - offset);
      for (size_t i = 0; i < count; ++i) {
        line[2 * i] = hex[pixels[offset + i] >> 4];
        line[2 * i + 1] = hex[pixels[offset + i] & 15];
      }
      line[count * 2] = 0;
      printf("LVGL_DATA %s\n", line);
    }
    printf("LVGL_END\n");
  }
}
bool LiveVoice::close() {
  live_esp32_preconnect(false);
  live_esp32_stop();
  if (timer_) {
    lv_timer_delete(timer_);
    timer_ = nullptr;
  }
  title_ = status_ = usage_ = transcript_area_ = control_ = control_text_ =
      nullptr;
  usage_note_ = nullptr;
  for (auto &label : usage_values_)
    label = nullptr;
  for (auto &label : transcripts_)
    label = nullptr;
  transcript_revision_ = UINT32_MAX;
  control_mode_ = -1;
  return true;
}
bool LiveVoice::back() {
  live_esp32_preconnect(false);
  live_esp32_stop();
  return notifyCoreClosed();
}
bool LiveVoice::pause() {
  live_esp32_preconnect(false);
  live_esp32_stop();
  if (timer_)
    lv_timer_pause(timer_);
  return true;
}
bool LiveVoice::resume() {
  live_esp32_preconnect(true);
  if (timer_)
    lv_timer_resume(timer_);
  return true;
}
bool LiveVoice::deinit() { return close(); }
} // namespace esp_brookesia::apps
