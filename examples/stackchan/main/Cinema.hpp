#pragma once
#include "esp_err.h"
#include <cstddef>
namespace cinema {
// Optional pack; no pack means normal StackChan captions and no extra tasks.
void before_display();
void init(const char *api_key);
void tick();
bool command(const char *line);
bool available();
// Called from the existing tool worker. Enqueues a bounded cue; never does I/O.
esp_err_t cue(const char *arguments, char *result, size_t capacity);
} // namespace cinema
