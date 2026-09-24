#ifndef SIM_TEST_CONTROL_H
#define SIM_TEST_CONTROL_H

// Simulator test-control state shared by the RPC handlers (socket thread)
// and the OS (main thread): the sequenced log ring, the app-launch slot,
// the last app's outcome and the --test-mode flag. The OS-facing hooks are
// declared in src/os/sim_hooks.h.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os/sim_hooks.h"

// ── Log ring ────────────────────────────────────────────────────────────────
// Sources: "lua" (sys.log), "native" (native sys->log), "os" (launcher
// events), "err" (error screens, launch refusals, SD escapes).
#define SIM_LOG_LINES 4096
#define SIM_LOG_TEXT_MAX 1024

// Append one entry; text containing '\n' becomes one entry per line. Every
// entry gets the next monotonic seq and is pushed as a `log` notification
// to subscribed clients.
void sim_log_append_src(const char *src, const char *text);
// Legacy entry point for Lua sys.log (source "lua").
void sim_log_append(const char *line);
// Drop every stored entry. Sequence numbers keep counting.
void sim_log_clear(void);
// JSON object {"lines":[{seq,t_ms,src,text}],"next_seq":N,"dropped":D,
// "more":bool,"count":held} for entries with seq >= since_seq. With tail > 0 only the
// newest `tail` entries at or after since_seq are returned. Caller frees.
char *sim_log_build_json(uint32_t since_seq, uint32_t tail);

// ── Launch slot ─────────────────────────────────────────────────────────────
// Socket thread: queue `name` for the main thread. A request that arrives
// before the previous one was taken replaces it (returns true in *replaced).
void sim_launch_request(const char *name, bool *replaced);
// Main thread: take the queued name (malloc'd, caller frees) or NULL.
char *sim_launch_take(void);
// Socket thread: ask the main thread to rescan /apps.
void sim_rescan_request(void);

// ── Test mode ───────────────────────────────────────────────────────────────
void sim_set_test_mode(bool on);

// ── JSON helper ─────────────────────────────────────────────────────────────
// Append `s` JSON-escaped (without quotes) to a growable buffer.
typedef struct {
  char *data;
  size_t len;
  size_t cap;
  bool oom;
} sim_strbuf_t;

void sim_sb_append(sim_strbuf_t *sb, const char *s, size_t n);
void sim_sb_appendf(sim_strbuf_t *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void sim_sb_append_json_str(sim_strbuf_t *sb, const char *s);  // with quotes
// Returns the buffer (caller frees) or NULL on OOM.
char *sim_sb_finish(sim_strbuf_t *sb);

#endif // SIM_TEST_CONTROL_H
