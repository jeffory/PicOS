#include "crashlog.h"
#include "lua_psram_alloc.h"
#include "../drivers/sdcard.h"

#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

#define ERROR_LOG_PATH   "/system/error.log"
#define CRASH_LOG_PATH   "/system/crashlog.txt"
#define RUNNING_PATH     "/system/running.txt"

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

// Open a log for append, truncating first if it has grown past the cap.
static sdfile_t open_log(const char *path) {
  if (!sdcard_is_mounted()) return NULL;
  truncate_if_large(path);
  return sdcard_fopen(path, "a");
}

static void write_str(sdfile_t f, const char *s) {
  if (s && *s) sdcard_fwrite(f, s, (int)strlen(s));
}

void crashlog_describe_heap(char *buf, size_t len) {
  if (!buf || len == 0) return;
  snprintf(buf, len, "free=%luK largest=%luK frag=%d%%",
           (unsigned long)(lua_psram_alloc_free_size() / 1024u),
           (unsigned long)(lua_psram_alloc_largest_block() / 1024u),
           lua_psram_alloc_fragmentation());
}

void crashlog_write(const char *kind, const char *app_name,
                    const char *context, const char *message) {
  if (!kind)     kind     = "ERROR";
  if (!app_name) app_name = "unknown";
  if (!context)  context  = "error";
  if (!message)  message  = "unknown error";

  char heap[96];
  crashlog_describe_heap(heap, sizeof(heap));

  // Always echo to serial so the record exists even when the SD write fails.
  printf("[%s] %s: %s: %s (%s)\n", kind, app_name, context, message, heap);

  sdfile_t f = open_log(ERROR_LOG_PATH);
  if (!f) return;

  char hdr[160];
  int hlen = snprintf(hdr, sizeof(hdr), "--- %s [%s] ---\n%s\n", kind, app_name,
                      context);
  sdcard_fwrite(f, hdr, hlen);
  write_str(f, message);
  write_str(f, "\nHeap: ");
  write_str(f, heap);
  write_str(f, "\n\n");
  sdcard_fclose(f);
}

void crashlog_write_lua_error(const char *app_name, const char *context,
                              const char *message) {
  crashlog_write("LUA ERROR", app_name, context, message);
}

// ── Dirty-exit marker ────────────────────────────────────────────────────────

void crashlog_mark_running(const char *app_id, const char *app_name,
                           const char *app_type) {
  if (!sdcard_is_mounted()) return;
  sdfile_t f = sdcard_fopen(RUNNING_PATH, "w");
  if (!f) return;

  char heap[96];
  crashlog_describe_heap(heap, sizeof(heap));

  char buf[320];
  int n = snprintf(buf, sizeof(buf),
                   "app=%s\nid=%s\ntype=%s\nlaunched_at=%lus\nheap=%s\n",
                   app_name ? app_name : "?", app_id ? app_id : "?",
                   app_type ? app_type : "?",
                   (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000u),
                   heap);
  if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
  sdcard_fwrite(f, buf, n);
  sdcard_fclose(f);
}

void crashlog_clear_running(void) {
  if (!sdcard_is_mounted()) return;
  if (sdcard_fsize(RUNNING_PATH) >= 0)
    sdcard_delete(RUNNING_PATH);
}

bool crashlog_read_running(char *out_name, size_t name_len,
                           char *out_detail, size_t detail_len) {
  if (out_name && name_len) out_name[0] = '\0';
  if (out_detail && detail_len) out_detail[0] = '\0';
  if (!sdcard_is_mounted()) return false;

  int size = sdcard_fsize(RUNNING_PATH);
  if (size < 0) return false;

  char text[320];
  int n = 0;
  sdfile_t f = sdcard_fopen(RUNNING_PATH, "r");
  if (f) {
    n = sdcard_fread(f, text, (int)sizeof(text) - 1);
    sdcard_fclose(f);
  }
  if (n < 0) n = 0;
  text[n] = '\0';

  if (out_name && name_len) {
    const char *p = strstr(text, "app=");
    if (p) {
      p += 4;
      size_t i = 0;
      while (*p && *p != '\n' && i < name_len - 1) out_name[i++] = *p++;
      out_name[i] = '\0';
    }
  }
  if (out_detail && detail_len)
    snprintf(out_detail, detail_len, "%s", text);
  return true;
}

void crashlog_write_unclean_exit(const char *reason, const char *detail) {
  if (!reason) reason = "unknown";
  sdfile_t f = open_log(CRASH_LOG_PATH);
  if (!f) return;

  char hdr[128];
  int hlen = snprintf(hdr, sizeof(hdr), "--- UNCLEAN EXIT ---\n  Reason: %s\n",
                      reason);
  sdcard_fwrite(f, hdr, hlen);

  // Indent each marker line under the header.
  if (detail) {
    const char *p = detail;
    while (*p) {
      const char *nl = strchr(p, '\n');
      size_t len = nl ? (size_t)(nl - p) : strlen(p);
      if (len) {
        sdcard_fwrite(f, "  ", 2);
        sdcard_fwrite(f, p, (int)len);
        sdcard_fwrite(f, "\n", 1);
      }
      if (!nl) break;
      p = nl + 1;
    }
  }
  sdcard_fwrite(f, "\n", 1);
  sdcard_fclose(f);
}
