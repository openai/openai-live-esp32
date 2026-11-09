#include "CinemaStorage.hpp"
#ifdef STACKCHAN_CINEMA
#include "CinemaAssets.hpp"
#include "bsp/m5stack_core_s3.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
esp_lcd_panel_io_handle_t lcd_io = nullptr;
bool ready = false;
// CoreS3 uses GPIO35 for both LCD D/C (output) and SD MISO (input).
// Holding the LVGL lock prevents new transfers; a synchronous NOP first drains
// the LCD IO queue. Restore the output before releasing the lock, even on
// errors.
struct CardAccess {
  bool locked = false, ok = false;
  CardAccess() {
    if (lcd_io) {
      if (!bsp_display_lock(100))
        return;
      locked = true;
      if (esp_lcd_panel_io_tx_param(lcd_io, 0x00, nullptr, 0) != ESP_OK)
        return;
    }
    ok = gpio_set_direction(BSP_SD_SPI_MISO, GPIO_MODE_INPUT) == ESP_OK;
  }
  ~CardAccess() {
    if (locked) {
      gpio_set_direction(BSP_LCD_DC, GPIO_MODE_OUTPUT);
      bsp_display_unlock();
    }
  }
};
} // namespace

// Capture the BSP-created LCD IO handle without editing downloaded vendor code.
extern "C" esp_err_t
__real_esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t,
                                const esp_lcd_panel_io_spi_config_t *,
                                esp_lcd_panel_io_handle_t *);
extern "C" esp_err_t
__wrap_esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                const esp_lcd_panel_io_spi_config_t *config,
                                esp_lcd_panel_io_handle_t *out) {
  const auto result = __real_esp_lcd_new_panel_io_spi(bus, config, out);
  if (result == ESP_OK && config->cs_gpio_num == BSP_LCD_CS &&
      config->dc_gpio_num == BSP_LCD_DC)
    lcd_io = *out;
  return result;
}
namespace cinema {
void storage_init() {
  // Deselect both devices before SD initialization on their shared bus.
  gpio_set_direction(BSP_LCD_CS, GPIO_MODE_OUTPUT);
  gpio_set_level(BSP_LCD_CS, 1);
  gpio_set_direction(BSP_SD_SPI_CS, GPIO_MODE_OUTPUT);
  gpio_set_level(BSP_SD_SPI_CS, 1);
  bsp_spi_cfg_t spi{};
  // The BSP SD default is only 4000 bytes; subsequent LCD setup reuses this
  // bus.
  spi.max_transfer_sz = 320 * 20 * 2;
  if (bsp_spi_init(&spi) != ESP_OK)
    return;
  esp_vfs_fat_sdmmc_mount_config_t mount{};
  mount.format_if_mount_failed = false;
  mount.max_files = 2;
  mount.allocation_unit_size = 16 * 1024;
  bsp_sdcard_cfg_t config{};
  config.mount = &mount;
  const auto result = bsp_sdcard_sdspi_mount(&config);
  if (result != ESP_OK) {
    ESP_LOGI("Cinema", "SD unavailable (%s); using flash preview",
             esp_err_to_name(result));
    return; // Do not unmount/free the SPI bus needed by the LCD.
  }
  FILE *file = fopen(BSP_SD_MOUNT_POINT "/CINEMA/INDEX.ID", "rb");
  if (file) {
    char digest[67]{};
    const auto size = fread(digest, 1, sizeof(digest), file);
    fclose(file);
    ready =
        size == 65 && digest[64] == '\n' && !memcmp(digest, assets::DIGEST, 64);
  }
  ESP_LOGI("Cinema", "SD index %s",
           ready ? "matches firmware"
                 : "missing or mismatched; using flash preview");
}
bool storage_available() { return ready; }
bool storage_read(unsigned id, uint8_t *output, size_t bytes) {
  if (!ready || id >= assets::COUNT || !output ||
      bytes != assets::CLIPS[id].bytes)
    return false;
  char path[80];
  snprintf(path, sizeof(path), BSP_SD_MOUNT_POINT "/CINEMA/%04u.SCF", id);
  FILE *file = nullptr;
  {
    CardAccess access;
    if (!access.ok)
      return false;
    file = fopen(path, "rb");
  }
  if (!file)
    return false;
  size_t offset = 0;
  while (offset < bytes) {
    size_t n = 0;
    {
      CardAccess access;
      if (access.ok)
        n = fread(output + offset, 1, std::min(size_t(4096), bytes - offset),
                  file);
    }
    if (!n)
      break;
    offset += n;
    // Short chunks keep the UI and Stop button serviceable while loading.
    vTaskDelay(1);
  }
  bool exact = false;
  {
    CardAccess access;
    if (access.ok)
      exact = offset == bytes && fgetc(file) == EOF && !ferror(file);
    // fopen used read-only mode; fclose never flushes data writes.
    fclose(file);
  }
  return exact;
}
} // namespace cinema
#endif
