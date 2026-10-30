#include "Board.hpp"
#include "TalkButton.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <algorithm>

namespace board {
static_assert(CONFIG_LIVE_ROUND_PTT_GPIO == -1 || CONFIG_LIVE_ROUND_PTT_GPIO == 0 ||
              (CONFIG_LIVE_ROUND_PTT_GPIO >= 16 &&
               CONFIG_LIVE_ROUND_PTT_GPIO <= 18),
              "Choose BOOT (0), an available expansion GPIO, or -1 to disable");
void button_init() {
  if (CONFIG_LIVE_ROUND_PTT_GPIO < 0) {
    ESP_LOGI("RoundButton", "Push-to-talk GPIO not configured");
    return;
  }
  static gpio_num_t pin = gpio_num_t(CONFIG_LIVE_ROUND_PTT_GPIO);
  gpio_config_t input{};
  input.pin_bit_mask = 1ULL << pin;
  input.mode = GPIO_MODE_INPUT;
  input.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK(gpio_config(&input));
  ESP_ERROR_CHECK(xTaskCreate([](void *context) {
    const auto pin = *static_cast<gpio_num_t *>(context);
    TalkButton button;
    for (;;) {
      if (button.update(gpio_get_level(pin) == 0,
                        esp_timer_get_time() / 1000)) {
        ESP_LOGI("RoundButton", "%s GPIO%d %s",
                 pin == GPIO_NUM_0 ? "BOOT" : "external", int(pin),
                 button.held ? "pressed" : "released");
        if (button.held)
          live_esp32_enable_push_to_talk();
        // Always release, including after Wi-Fi loss. A press without Wi-Fi
        // must be retried once connected; don't silently start later.
        if (!button.held || wifi_ready())
          live_esp32_talk(button.held);
      }
      vTaskDelay(std::max(TickType_t(1), pdMS_TO_TICKS(5)));
    }
  }, "live_button", 4096, &pin, 5, nullptr) == pdPASS
      ? ESP_OK : ESP_ERR_NO_MEM);
}
} // namespace board
