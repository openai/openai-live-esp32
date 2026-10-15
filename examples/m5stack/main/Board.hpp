#pragma once
#include "live_esp32.h"

namespace board {
void audio_init();
esp_err_t audio_open(void *);
void audio_close(void *);
esp_err_t audio_read(void *, int16_t *, size_t);
esp_err_t audio_write(void *, const int16_t *, size_t);
uint32_t audio_starvations(void *);
void wifi_init();
bool wifi_ready();
void wifi_next();
void wifi_status();
void view_init();
void view_update();
} // namespace board
