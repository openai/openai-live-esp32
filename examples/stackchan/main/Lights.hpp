#pragma once
#include "driver/i2c_master.h"
namespace stackchan {
// Uses the detected base's existing handle; never changes motor power or NVS.
void lights_init(i2c_master_dev_handle_t base);
void lights_poll(); // Main/housekeeping task only, never the audio callback.
bool lights_command(const char *line);
} // namespace stackchan
