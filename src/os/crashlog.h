#ifndef CRASHLOG_H
#define CRASHLOG_H

#include <stdbool.h>
#include <stddef.h>

// =============================================================================
// Persistent error / crash records on the SD card
//
//   /system/error.log    — app-level failures written while the OS is alive:
//                          Lua runtime errors, Lua panics, native loader
//                          errors, launch refusals.  Every entry carries a
//                          heap line (free / largest block / fragmentation).
//   /system/crashlog.txt — OS-level records written at boot: HardFault dumps
//                          (from watchdog scratch) and unclean-exit reports.
//   /system/running.txt  — dirty-exit marker.  Written when an app launches,
//                          deleted when the runner returns.  If it still
//                          exists at boot the previous app never exited
//                          cleanly (hang, watchdog, panic, power loss).
//
// All functions are safe from normal thread context only (they use the SD
// card API, which allocates from the PSRAM heap).  Never call from an ISR or
// the HardFault handler.
// =============================================================================

// Append an entry to /system/error.log.
//   --- <kind> [<app_name>] ---
//   <context>
//   <message>
//   Heap: free=... largest=... frag=...
void crashlog_write(const char *kind, const char *app_name,
                    const char *context, const char *message);

// Convenience wrapper: crashlog_write("LUA ERROR", ...).
void crashlog_write_lua_error(const char *app_name, const char *context,
                              const char *message);

// Fill buf with a one-line PSRAM heap summary ("free=5832K largest=5820K frag=3%").
void crashlog_describe_heap(char *buf, size_t len);

// ── Dirty-exit marker ────────────────────────────────────────────────────────

// Write /system/running.txt for the app about to run.
void crashlog_mark_running(const char *app_id, const char *app_name,
                           const char *app_type);

// Delete /system/running.txt.  Call when the runner returns, and before any
// intentional reboot issued while an app is running.
void crashlog_clear_running(void);

// At boot: if /system/running.txt exists, copy the app name into out_name and
// the whole marker text into out_detail (either may be NULL) and return true.
// Does not delete the marker.
bool crashlog_read_running(char *out_name, size_t name_len,
                           char *out_detail, size_t detail_len);

// Append an "--- UNCLEAN EXIT ---" entry to /system/crashlog.txt.
void crashlog_write_unclean_exit(const char *reason, const char *detail);

#endif // CRASHLOG_H
