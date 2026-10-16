#include "Board.hpp"
#include "CaptionText.hpp"
#include "LiveFonts.h"
#include "TranscriptPage.hpp"
#include "UsageSummary.hpp"
#include "bsp/m5stack_core_s3.h"
#include "src/misc/lv_text_private.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

static_assert(LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR == 6 &&
                  LVGL_VERSION_PATCH == 0,
              "Build with the LVGL version pinned in idf_component.yml");

namespace board {
namespace {
constexpr int CONTENT_HEIGHT = 176, LINE_WIDTH = 292;
const lv_font_t *font = &live_font_18;
lv_obj_t *welcome, *status_label, *captions, *usage, *button, *button_text;
lv_obj_t *lines[10], *dots[10], *values[live_usage_view::ROW_COUNT];
live_caption_view::Page page;
live_transcript_t snapshot{};
uint32_t revision = UINT32_MAX;
void show(lv_obj_t *object, bool visible) {
  if (visible)
    lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}
void text(lv_obj_t *label, const char *value) {
  if (strcmp(lv_label_get_text(label), value))
    lv_label_set_text(label, value);
}
lv_obj_t *area(lv_obj_t *parent, int x, int y, int w, int h) {
  auto *obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, w, h);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  return obj;
}
lv_obj_t *label(lv_obj_t *parent, int x, int y, int w, uint32_t color) {
  auto *obj = lv_label_create(parent);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_width(obj, w);
  lv_obj_set_style_text_font(obj, font, 0);
  lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
  lv_obj_set_style_text_line_space(obj, 0, 0);
  lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
  lv_label_set_text(obj, "");
  return obj;
}
bool glyph(uint32_t codepoint) {
  lv_font_glyph_dsc_t dsc{};
  return font->get_glyph_dsc(font, &dsc, codepoint, 0) && !dsc.is_placeholder;
}
void press(lv_event_t *) {
  live_esp32_status_t state{};
  live_esp32_status(&state);
  if (state.busy)
    live_esp32_stop();
  else if (wifi_ready())
    live_esp32_start();
}
void render_captions() {
  live_esp32_transcripts(&snapshot);
  live_caption_view::simplify(snapshot, glyph);
  auto next_line = [&](const char *value) {
    lv_text_attributes_t attributes{};
    attributes.max_width = LINE_WIDTH;
    return lv_text_get_next_line(value, LV_TEXT_LEN_MAX, font, nullptr,
                                 &attributes);
  };
  page.update(snapshot, CONTENT_HEIGHT, font->line_height, 0, 2, next_line);
  unsigned index = 0;
  int y = 0;
  for (unsigned i = 0; i < page.count; ++i) {
    const auto &block = page.blocks[i];
    const auto color =
        lv_color_hex(block.speaker == LIVE_SPEAKER_USER ? 0x83D9E5 : 0xE5C78F);
    size_t offset = 0, length = strlen(block.text);
    bool first = true;
    for (;;) {
      if (index >= std::size(lines))
        break;
      const size_t n = offset < length
                           ? std::min(size_t(next_line(block.text + offset)),
                                      length - offset)
                           : 0;
      char value[LIVE_TRANSCRIPT_BYTES];
      memcpy(value, block.text + offset, n);
      size_t end = n;
      while (end && (value[end - 1] == '\n' || value[end - 1] == '\r' ||
                     value[end - 1] == ' '))
        --end;
      value[end] = 0;
      text(lines[index], value);
      lv_obj_set_y(lines[index], y);
      lv_obj_set_style_text_color(lines[index], color, 0);
      show(lines[index], true);
      lv_obj_set_y(dots[index], y + font->line_height / 2 - 2);
      lv_obj_set_style_bg_color(dots[index],
                                first ? color : lv_color_hex(0x243236), 0);
      show(dots[index], true);
      first = false;
      ++index;
      y += font->line_height;
      if (!n)
        break;
      offset += n;
      if (offset == length && block.text[offset - 1] != '\n' &&
          block.text[offset - 1] != '\r')
        break;
    }
    y += 2;
  }
  for (; index < std::size(lines); ++index) {
    show(lines[index], false);
    show(dots[index], false);
  }
  revision = snapshot.revision;
}
} // namespace
void view_init() {
  bsp_display_cfg_t config{};
  config.lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  config.lvgl_port_cfg.task_priority = 3;
  config.lvgl_port_cfg.task_affinity = 1;
  config.buffer_size = 320 * 20;
  config.double_buffer = true;
  config.flags.buff_dma = true;
  ESP_ERROR_CHECK(bsp_display_start_with_config(&config) ? ESP_OK : ESP_FAIL);
  ESP_ERROR_CHECK(bsp_display_brightness_set(85));
  ESP_ERROR_CHECK(bsp_display_lock(0) ? ESP_OK : ESP_FAIL);
  auto *screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  welcome = label(screen, 20, 55, 280, 0xE5E9EB);
  text(welcome, "Live Voice");
  lv_obj_set_style_text_align(welcome, LV_TEXT_ALIGN_CENTER, 0);
  status_label = label(screen, 20, 90, 280, 0x82949B);
  lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
  captions = area(screen, 8, 8, 304, CONTENT_HEIGHT);
  usage = area(screen, 12, 10, 296, CONTENT_HEIGHT);
  for (unsigned i = 0; i < std::size(lines); ++i) {
    lines[i] = label(captions, 12, 0, LINE_WIDTH, 0xE5C78F);
    lv_obj_set_height(lines[i], font->line_height);
    dots[i] = area(captions, 1, 0, 3, 3);
    lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
    show(lines[i], false);
    show(dots[i], false);
  }
  for (int i = 0; i < live_usage_view::ROW_COUNT; ++i) {
    text(label(usage, 0, i * 26, 88, 0x82949B), live_usage_view::KEYS[i]);
    values[i] = label(usage, 90, i * 26, 206, 0xE5C78F);
    lv_obj_set_style_text_font(values[i], &live_font_16, 0);
  }
  button = lv_button_create(screen);
  lv_obj_set_size(button, 156, 40);
  lv_obj_set_pos(button, 82, 193);
  lv_obj_set_style_radius(button, 20, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_ext_click_area(button, 10);
  lv_obj_add_event_cb(button, press, LV_EVENT_CLICKED, nullptr);
  button_text = label(button, 0, 0, LV_SIZE_CONTENT, 0x8CDBAD);
  lv_obj_center(button_text);
  show(captions, false);
  show(usage, false);
  bsp_display_unlock();
  view_update();
}
void view_update() {
  live_esp32_status_t state{};
  live_esp32_status(&state);
  if (!bsp_display_lock(100))
    return;
  const bool transcripts = state.active;
  const bool final = !state.busy && state.usage.started;
  show(welcome, !transcripts && !final);
  show(status_label, !transcripts && !final);
  show(captions, transcripts);
  show(usage, final);
  text(status_label, wifi_ready() ? state.status : "Connecting Wi-Fi...");
  if (transcripts && revision != state.transcript_revision)
    render_captions();
  if (final) {
    char value[160];
    for (int i = 0; i < live_usage_view::ROW_COUNT; ++i) {
      live_usage_view::format(state.usage, live_usage_view::Row(i), value,
                              sizeof(value));
      text(values[i], value);
    }
  }
  const uint32_t color = state.active   ? 0xE89CA0
                         : state.busy   ? 0xE5C78F
                         : wifi_ready() ? 0x8CDBAD
                                        : 0x82949B;
  text(button_text, state.busy ? "Stop" : "Start");
  lv_obj_center(button_text);
  lv_obj_set_style_text_color(button_text, lv_color_hex(color), 0);
  lv_obj_set_style_bg_color(
      button, lv_color_mix(lv_color_hex(color), lv_color_black(), 32), 0);
  lv_obj_set_style_border_color(
      button, lv_color_mix(lv_color_hex(color), lv_color_black(), 64), 0);
  if (wifi_ready() || state.busy)
    lv_obj_remove_state(button, LV_STATE_DISABLED);
  else
    lv_obj_add_state(button, LV_STATE_DISABLED);
  bsp_display_unlock();
}
} // namespace board
