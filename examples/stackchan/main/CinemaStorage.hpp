#pragma once
#include <cstddef>
#include <cstdint>
namespace cinema {
void storage_init(); // Before display creation; never formats or writes the
                     // card.
bool storage_available();
bool storage_read(unsigned id, uint8_t *output, size_t bytes);
} // namespace cinema
