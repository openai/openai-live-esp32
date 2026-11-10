#include "Board.hpp"
#include "ToolPolicy.hpp"
#include "bsp/esp_mosaico.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "mosaico_module_interact.h"
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace board {
namespace {
SemaphoreHandle_t lock;
mosaico_interact_handle_t interaction = nullptr;
bool imu = false, battery = false, magnetic = false, motor = false;
mosaico::ButtonEdge left_button, right_button;
int64_t lights_until = 0;
int last_light = -1;
mosaico_interact_inputs_t inputs{};
constexpr char TOOLS[] = R"json([
{"type":"function","name":"read_device_sensors","description":"Read fresh Mosaico IMU (g, degrees/second), two magnetometers, battery, interaction buttons, PIR motion and relative ambient light (0-100, not lux). Unavailable sensors return null.","parameters":{"type":"object","properties":{},"required":[],"additionalProperties":false},"strict":true},
{"type":"function","name":"set_lights","description":"Set one of six interaction LEDs or all (index -1). Brightness is hardware-limited. Returns to session status after the duration or Stop.","parameters":{"type":"object","properties":{"index":{"type":"integer","minimum":-1,"maximum":5},"red":{"type":"integer","minimum":0,"maximum":255},"green":{"type":"integer","minimum":0,"maximum":255},"blue":{"type":"integer","minimum":0,"maximum":255},"duration_ms":{"type":"integer","minimum":100,"maximum":10000}},"required":["index","red","green","blue","duration_ms"],"additionalProperties":false},"strict":true},
{"type":"function","name":"vibrate","description":"A short haptic pulse, at most 500 milliseconds. Stops immediately when the session is cancelled.","parameters":{"type":"object","properties":{"strength":{"type":"integer","minimum":1,"maximum":100},"duration_ms":{"type":"integer","minimum":10,"maximum":500}},"required":["strength","duration_ms"],"additionalProperties":false},"strict":true},
{"type":"function","name":"send_infrared","description":"Send ONE standard NEC 38 kHz frame. Only use when the user explicitly requests infrared control and supplies a known 8-bit device address and command; never scan or guess codes.","parameters":{"type":"object","properties":{"address":{"type":"integer","minimum":0,"maximum":255},"command":{"type":"integer","minimum":0,"maximum":255}},"required":["address","command"],"additionalProperties":false},"strict":true}
])json";
using mosaico::integer;
using mosaico::keys;
void vector(cJSON *obj, const char *key, float x, float y, float z,
            bool valid) {
  if (!valid || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
    cJSON_AddNullToObject(obj, key);
    return;
  }
  double values[]{x, y, z};
  cJSON_AddItemToObject(obj, key, cJSON_CreateDoubleArray(values, 3));
}
esp_err_t sensors(char *out, size_t capacity) {
  cJSON *obj = cJSON_CreateObject();
  if (!obj)
    return ESP_ERR_NO_MEM;
  float x = 0, y = 0, z = 0;
  bool ok = imu && bsp_imu_get_accel(&x, &y, &z) == ESP_OK;
  vector(obj, "acceleration_g", x, y, z, ok);
  ok = imu && bsp_imu_get_gyro(&x, &y, &z) == ESP_OK;
  vector(obj, "angular_velocity_dps", x, y, z, ok);
  for (unsigned i = 0; i < 2; ++i) {
    bmm150_mag_data m{};
    ok = magnetic && bsp_magnetometer_read(bsp_magnetometer_t(i), &m) == ESP_OK;
    vector(obj, i ? "magnetometer_right" : "magnetometer_left", m.x, m.y, m.z,
           ok);
  }
  bsp_battery_status_t power{};
  if (battery && bsp_battery_read(&power) == ESP_OK) {
    cJSON_AddNumberToObject(obj, "battery_percent", power.state_of_charge);
    cJSON_AddNumberToObject(obj, "battery_millivolts", power.voltage_mv);
    cJSON_AddNumberToObject(obj, "battery_temperature_c", power.temperature_c);
  } else {
    cJSON_AddNullToObject(obj, "battery_percent");
  }
  mosaico_interact_inputs_t in{};
  if (interaction && mosaico_interact_read_inputs(interaction, &in) == ESP_OK) {
    cJSON_AddNumberToObject(obj, "relative_light", in.light_level);
    cJSON_AddBoolToObject(obj, "motion_detected", in.motion_detected);
    cJSON_AddBoolToObject(obj, "left_pressed", in.left_pressed);
    cJSON_AddBoolToObject(obj, "right_pressed", in.right_pressed);
  } else {
    cJSON_AddNullToObject(obj, "interaction_module");
  }
  ok = capacity <= INT_MAX &&
       cJSON_PrintPreallocated(obj, out, int(capacity), false);
  cJSON_Delete(obj);
  return ok ? ESP_OK : ESP_ERR_NO_MEM;
}
void ai_press(void *, void *) {
  ESP_LOGI("MosaicoHW", "AI button pressed");
  toggle_session();
}
} // namespace

const char *tools_json() { return TOOLS; }
void toggle_session() {
  live_esp32_status_t state{};
  live_esp32_status(&state);
  if (state.busy)
    live_esp32_stop();
  else if (wifi_ready())
    live_esp32_start();
}
void hardware_init() {
  lock = xSemaphoreCreateMutex();
  ESP_ERROR_CHECK(lock ? ESP_OK : ESP_ERR_NO_MEM);
  motor = bsp_motor_init() == ESP_OK;
  if (motor)
    bsp_motor_set(false);
  imu = bsp_imu_init() == ESP_OK && bsp_imu_start(nullptr) == ESP_OK;
  battery = bsp_battery_init() == ESP_OK;
  magnetic = bsp_magnetometer_init_all() == ESP_OK;
  mosaico_interact_config_t config = MOSAICO_INTERACT_DEFAULT_CONFIG();
  // Discovery debounces both slots over multiple 250 ms scans.
  config.discovery_timeout_ms = 2000;
  auto err = mosaico_interact_open(&config, &interaction);
  ESP_LOGI("MosaicoHW", "IMU=%d battery=%d magnets=%d haptic=%d interaction=%s",
           imu, battery, magnetic, motor, esp_err_to_name(err));
  button_handle_t button[BSP_BUTTON_NUM]{};
  int count = 0;
  if (bsp_iot_button_create(button, &count, BSP_BUTTON_NUM) == ESP_OK)
    ESP_ERROR_CHECK(iot_button_register_cb(button[BSP_BUTTON_AI],
                                           BUTTON_SINGLE_CLICK, nullptr,
                                           ai_press, nullptr));
}
void hardware_poll() {
  // Never wait behind a camera or a long tool. No PIR/light event opens the
  // mic.
  if (!interaction || xSemaphoreTake(lock, 0) != pdTRUE)
    return;
  live_esp32_status_t state{};
  live_esp32_status(&state);
  if (mosaico_interact_read_inputs(interaction, &inputs) == ESP_OK) {
    const auto now = esp_timer_get_time();
    if (left_button.update(inputs.left_pressed, now)) {
      ESP_LOGI("MosaicoHW", "Interaction left pressed");
      toggle_session();
    }
    if (right_button.update(inputs.right_pressed, now) && state.busy) {
      ESP_LOGI("MosaicoHW", "Interaction right pressed");
      live_esp32_mute(!state.muted);
    }
  }
  if (!state.busy)
    lights_until = 0;
  if (esp_timer_get_time() >= lights_until) {
    int indicator = state.active ? (state.muted ? 3 : 2) : state.busy ? 1 : 0;
    if (indicator != last_light) {
      mosaico_interact_rgb_t color{};
      if (indicator == 1)
        color = {255, 160, 30};
      if (indicator == 2)
        color = {20, 210, 255};
      if (indicator == 3)
        color = {255, 40, 70};
      if (mosaico_interact_led_fill(interaction, color) == ESP_OK)
        last_light = indicator;
    }
  }
  xSemaphoreGive(lock);
}
esp_err_t execute_tool(void *, const char *name, const char *arguments,
                       char *result, size_t capacity, bool (*cancelled)(void *),
                       void *context) {
  if (!name || !arguments || !result || capacity < 64 || !cancelled)
    return ESP_ERR_INVALID_ARG;
  result[0] = 0;
  cJSON *args = mosaico::parse_arguments(arguments);
  if (!args)
    return ESP_ERR_INVALID_ARG;
  int a = 0, b = 0, c = 0, d = 0, ms = 0;
  const char *led_keys[]{"index", "red", "green", "blue", "duration_ms"};
  const char *motor_keys[]{"strength", "duration_ms"};
  const char *ir_keys[]{"address", "command"};
  bool read = !strcmp(name, "read_device_sensors") && keys(args, nullptr, 0);
  bool leds =
      !strcmp(name, "set_lights") && keys(args, led_keys, 5) &&
      integer(args, "index", -1, 5, a) && integer(args, "red", 0, 255, b) &&
      integer(args, "green", 0, 255, c) && integer(args, "blue", 0, 255, d) &&
      integer(args, "duration_ms", 100, 10000, ms);
  bool pulse = !strcmp(name, "vibrate") && keys(args, motor_keys, 2) &&
               integer(args, "strength", 1, 100, a) &&
               integer(args, "duration_ms", 10, 500, ms);
  bool ir = !strcmp(name, "send_infrared") && keys(args, ir_keys, 2) &&
            integer(args, "address", 0, 255, a) &&
            integer(args, "command", 0, 255, b);
  cJSON_Delete(args);
  if (!(read || leds || pulse || ir))
    return ESP_ERR_INVALID_ARG;
  if (cancelled(context))
    return ESP_ERR_INVALID_STATE;
  if (xSemaphoreTake(lock, pdMS_TO_TICKS(100)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  esp_err_t err = ESP_ERR_NOT_FOUND;
  if (cancelled(context)) {
    err = ESP_ERR_INVALID_STATE;
  } else if (read) {
    err = sensors(result, capacity);
  } else if (leds && interaction) {
    mosaico_interact_rgb_t color{uint8_t(b), uint8_t(c), uint8_t(d)};
    err = a < 0 ? mosaico_interact_led_fill(interaction, color)
                : mosaico_interact_led_set(interaction, uint8_t(a), color);
    if (err == ESP_OK) {
      lights_until = esp_timer_get_time() + int64_t(ms) * 1000;
      last_light = -1;
    }
  } else if (pulse && motor) {
    err = bsp_motor_set_strength(a);
    const auto until = esp_timer_get_time() + int64_t(ms) * 1000;
    while (err == ESP_OK && esp_timer_get_time() < until) {
      if (cancelled(context)) {
        err = ESP_ERR_INVALID_STATE;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    const auto stopped = bsp_motor_set(false);
    if (err == ESP_OK)
      err = stopped;
  } else if (ir && interaction) {
    err = mosaico_interact_ir_send_nec(interaction, a, b);
  }
  xSemaphoreGive(lock);
  if (!read)
    snprintf(result, capacity, "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
  return err;
}
} // namespace board
