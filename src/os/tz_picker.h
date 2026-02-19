#pragma once
#include <stdbool.h>

// Show the timezone picker overlay.
// On confirmation, writes the selected offset (in minutes) to config key
// "tz_offset" and calls config_save(). Returns true if changed, false if cancelled.
bool tz_picker_show(void);
