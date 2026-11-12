#pragma once
#include <cstddef>
#include <cstdint>
namespace cinema {
void storage_init(); // Never formats the card or erases existing directories.
bool storage_available();
enum class ReadResult { ok, unavailable, cancelled };
ReadResult storage_read(unsigned id, uint8_t *output, size_t bytes,
                        bool (*cancelled)());
bool storage_command(
    const char *line); // USB-only, while Live and cinema are idle.
} // namespace cinema
