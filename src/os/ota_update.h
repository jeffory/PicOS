#pragma once

#include <stdbool.h>
#include <stdint.h>

#define OTA_MAGIC       0x4F544100u  // "OTA\0"
#define OTA_SCRATCH_IDX 1            // Use scratch[1] — scratch[0] is boot-loop counter
#define OTA_BIN_PATH  "/system/update.bin"
#define OTA_HASH_PATH "/system/update.sha256"

// Check if an OTA update is pending (call very early in main).
// Reads watchdog scratch[0] for OTA_MAGIC.
bool ota_check_pending(void);

// Apply the pending update. Must be called BEFORE Core 1 launch.
// Requires: display_init() done (for progress), sdcard_init() done.
// This function does not return on success (it reboots).
// On failure, it clears the flag and returns false.
bool ota_apply_update(void);

// Trigger an OTA update: validate file, set scratch register, reboot.
// Called from Lua bridge. Does not return on success.
// Returns false with error message in out_err on failure.
bool ota_trigger_update(const char *bin_path, const char **out_err);
