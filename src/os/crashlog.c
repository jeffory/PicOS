#include "crashlog.h"
#include "../drivers/sdcard.h"

#include <stdio.h>
#include <string.h>

// Maximum log file size before truncation (bytes).
#define CRASHLOG_MAX_SIZE (64u * 1024u)

// Truncate file to zero if it exceeds the size cap.
static void truncate_if_large(const char *path) {
  int size = sdcard_fsize(path);
  if (size > 0 && (uint32_t)size > CRASHLOG_MAX_SIZE) {
    sdfile_t f = sdcard_fopen(path, "w");
    if (f) sdcard_fclose(f);
  }
}

void crashlog_write_lua_error(const char *app_name, const char *context,
                              const char *message) {
  if (!app_name) app_name = "unknown";
  if (!context)  context  = "error";
  if (!message)  message  = "unknown error";

  if (!sdcard_is_mounted()) return;

  truncate_if_large("/system/error.log");

  sdfile_t f = sdcard_fopen("/system/error.log", "a");
  if (!f) return;

  char hdr[128];
  int hlen = snprintf(hdr, sizeof(hdr), "--- LUA ERROR [%s] ---\n%s\n",
                      app_name, context);
  sdcard_fwrite(f, hdr, hlen);
  int mlen = strlen(message);
  sdcard_fwrite(f, message, mlen);
  sdcard_fwrite(f, "\n\n", 2);
  sdcard_fclose(f);
}
