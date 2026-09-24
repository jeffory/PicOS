#pragma once

#include <stdbool.h>
#include <stdint.h>

#define OTA_MAGIC       0x4F544100u  // "OTA\0"
#define OTA_SCRATCH_IDX 1            // Use scratch[1] — scratch[0] is boot-loop counter
#define OTA_BIN_PATH  "/system/update.bin"
#define OTA_HASH_PATH "/system/update.sha256"
#define OTA_SIG_PATH  "/system/update.sig"

// Check if an OTA update is pending (call very early in main).
// Reads watchdog scratch[0] for OTA_MAGIC.
bool ota_check_pending(void);

// Apply the pending update. Must be called BEFORE Core 1 launch.
// Requires: display_init() done (for progress), sdcard_init() done.
// This function does not return on success (it reboots).
// On failure (including a bad checksum or signature) it clears the flag,
// shows the reason, logs it to /system/error.log, renames the staged files
// to *.rejected (never retried) and returns false.
bool ota_apply_update(void);

// Boot: /system/update.bin exists but no update was requested (no OTA
// token).  Rename it (and its .sha256/.sig) to *.stale so it is never
// flashed or retried, and log it to /system/error.log.
void ota_discard_unrequested(void);

// Validate an update without applying it: size, vector table, the
// mandatory OTA_HASH_PATH checksum and the OTA_SIG_PATH signature over the
// image (ota_verify.h).  The boot re-checks both over the bytes it flashes.
// Returns false with a message in out_err.
bool ota_prepare_update(const char *bin_path, const char **out_err);

// Trigger an OTA update: ota_prepare_update, set scratch register, reboot.
// Called from Lua bridge. Does not return on success.
// Returns false with error message in out_err on failure.
bool ota_trigger_update(const char *bin_path, const char **out_err);
