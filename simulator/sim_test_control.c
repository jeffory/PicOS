// Simulator test-control state: sequenced log ring, launch slot, app
// outcome and --test-mode. See sim_test_control.h and src/os/sim_hooks.h.

#define _GNU_SOURCE

#include "sim_test_control.h"
#include "sim_socket.h"
#include "sim_socket_handler.h"
#include "hal/hal_timing.h"
#include "os/launcher.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── Growable string buffer ──────────────────────────────────────────────────

static bool sb_reserve(sim_strbuf_t *sb, size_t extra) {
  if (sb->oom) return false;
  size_t need = sb->len + extra + 1;
  if (need <= sb->cap) return true;
  size_t cap = sb->cap ? sb->cap : 256;
  while (cap < need) cap *= 2;
  char *p = realloc(sb->data, cap);
  if (!p) {
    sb->oom = true;
    return false;
  }
  sb->data = p;
  sb->cap = cap;
  return true;
}

void sim_sb_append(sim_strbuf_t *sb, const char *s, size_t n) {
  if (!sb_reserve(sb, n)) return;
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = '\0';
}

void sim_sb_appendf(sim_strbuf_t *sb, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0 || !sb_reserve(sb, (size_t)n)) return;
  va_start(ap, fmt);
  vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
  va_end(ap);
  sb->len += (size_t)n;
}

void sim_sb_append_json_str(sim_strbuf_t *sb, const char *s) {
  sim_sb_append(sb, "\"", 1);
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
    switch (*p) {
    case '"': sim_sb_append(sb, "\\\"", 2); break;
    case '\\': sim_sb_append(sb, "\\\\", 2); break;
    case '\n': sim_sb_append(sb, "\\n", 2); break;
    case '\r': sim_sb_append(sb, "\\r", 2); break;
    case '\t': sim_sb_append(sb, "\\t", 2); break;
    default:
      if (*p < 0x20) sim_sb_appendf(sb, "\\u%04x", *p);
      else sim_sb_append(sb, (const char *)p, 1);
    }
  }
  sim_sb_append(sb, "\"", 1);
}

char *sim_sb_finish(sim_strbuf_t *sb) {
  if (sb->oom || !sb_reserve(sb, 0)) {
    free(sb->data);
    sb->data = NULL;
    return NULL;
  }
  return sb->data;
}

// ── Log ring ────────────────────────────────────────────────────────────────

typedef struct {
  uint32_t seq;
  uint32_t t_ms;
  char src[8];
  char text[SIM_LOG_TEXT_MAX];
} log_entry_t;

static log_entry_t s_log[SIM_LOG_LINES];
static int s_log_head = 0;       // index of the oldest entry
static int s_log_count = 0;
static uint32_t s_log_next_seq = 1;  // seq of the next entry; 0 is "from the start"
static pthread_mutex_t s_log_mutex = PTHREAD_MUTEX_INITIALIZER;

// Send one entry to subscribers. Called with s_log_mutex held so
// notifications leave in seq order.
static void notify_log_entry(const log_entry_t *e) {
  sim_strbuf_t sb = {0};
  sim_sb_appendf(&sb, "{\"jsonrpc\":\"2.0\",\"method\":\"log\",\"params\":{\"seq\":%u,\"src\":",
                 e->seq);
  sim_sb_append_json_str(&sb, e->src);
  sim_sb_append(&sb, ",\"text\":", 8);
  sim_sb_append_json_str(&sb, e->text);
  sim_sb_append(&sb, "}}\n", 3);
  char *line = sim_sb_finish(&sb);
  if (line) {
    sim_socket_notify_log_subscribers(line, strlen(line));
    free(line);
  }
}

static void log_append_one(const char *src, const char *text, size_t len) {
  if (len >= SIM_LOG_TEXT_MAX) len = SIM_LOG_TEXT_MAX - 1;
  uint32_t now = hal_get_time_ms();
  pthread_mutex_lock(&s_log_mutex);
  int idx = (s_log_head + s_log_count) % SIM_LOG_LINES;
  if (s_log_count < SIM_LOG_LINES) {
    s_log_count++;
  } else {
    s_log_head = (s_log_head + 1) % SIM_LOG_LINES;
  }
  log_entry_t *e = &s_log[idx];
  e->seq = s_log_next_seq++;
  e->t_ms = now;
  snprintf(e->src, sizeof(e->src), "%s", src);
  memcpy(e->text, text, len);
  e->text[len] = '\0';
  notify_log_entry(e);
  pthread_mutex_unlock(&s_log_mutex);
}

void sim_log_append_src(const char *src, const char *text) {
  if (!text) return;
  if (!*text) {
    log_append_one(src, "", 0);
    return;
  }
  // One entry per line; a trailing newline adds no empty entry.
  const char *p = text;
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    log_append_one(src, p, len);
    if (!nl) break;
    p = nl + 1;
  }
}

void sim_log_append(const char *line) { sim_log_append_src("lua", line); }

void sim_log_clear(void) {
  pthread_mutex_lock(&s_log_mutex);
  s_log_head = 0;
  s_log_count = 0;
  pthread_mutex_unlock(&s_log_mutex);
}

// Upper bound on one get_log_buffer response; larger results are paged
// ("more":true, continue from next_seq). The socket write buffer is 512 KB.
#define LOG_RESPONSE_BUDGET (256 * 1024)

char *sim_log_build_json(uint32_t since_seq, uint32_t tail) {
  sim_strbuf_t sb = {0};
  sim_sb_append(&sb, "{\"lines\":[", 10);

  pthread_mutex_lock(&s_log_mutex);
  uint32_t oldest = s_log_count ? s_log[s_log_head].seq : s_log_next_seq;
  uint32_t want = since_seq ? since_seq : 1;  // 0 means "from the start"
  uint32_t from = want > oldest ? want : oldest;
  // Entries in [want, oldest) are gone: evicted by the ring or cleared.
  uint32_t dropped = want < oldest ? oldest - want : 0;
  if (tail > 0 && s_log_next_seq > from && s_log_next_seq - from > tail)
    from = s_log_next_seq - tail;

  uint32_t next = s_log_next_seq;
  int held = s_log_count;
  bool more = false;
  bool first = true;
  for (int i = 0; i < s_log_count; i++) {
    const log_entry_t *e = &s_log[(s_log_head + i) % SIM_LOG_LINES];
    if (e->seq < from) continue;
    if (sb.len > LOG_RESPONSE_BUDGET) {
      more = true;
      next = e->seq;  // resume here
      break;
    }
    sim_sb_appendf(&sb, "%s{\"seq\":%u,\"t_ms\":%u,\"src\":", first ? "" : ",",
                   e->seq, e->t_ms);
    sim_sb_append_json_str(&sb, e->src);
    sim_sb_append(&sb, ",\"text\":", 8);
    sim_sb_append_json_str(&sb, e->text);
    sim_sb_append(&sb, "}", 1);
    first = false;
  }
  pthread_mutex_unlock(&s_log_mutex);

  // "count" (entries currently held) predates the seq API; kept for callers.
  sim_sb_appendf(&sb, "],\"next_seq\":%u,\"dropped\":%u,\"more\":%s,\"count\":%d}",
                 next, dropped, more ? "true" : "false", held);
  return sim_sb_finish(&sb);
}

static void log_vformat(const char *src, const char *fmt, va_list ap) {
  char buf[2048];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  sim_log_append_src(src, buf);
}

void sim_log_err(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  log_vformat("err", fmt, ap);
  va_end(ap);
}

void sim_log_os(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  log_vformat("os", fmt, ap);
  va_end(ap);
}

// ── Test mode ───────────────────────────────────────────────────────────────

static volatile bool s_test_mode = false;

void sim_set_test_mode(bool on) { s_test_mode = on; }
bool sim_test_mode(void) { return s_test_mode; }

// ── Launch slot ─────────────────────────────────────────────────────────────

static pthread_mutex_t s_launch_mutex = PTHREAD_MUTEX_INITIALIZER;
static char *s_launch_name = NULL;
static uint32_t s_launch_id = 0;       // id of the queued request
static uint32_t s_launch_next_id = 1;  // next id to hand out
static bool s_rescan_requested = false;
// Last app.exited params, kept for get_last_outcome (guarded by the mutex).
static char *s_last_outcome = NULL;

uint32_t sim_launch_request(const char *name, bool *replaced) {
  char *copy = strdup(name);
  pthread_mutex_lock(&s_launch_mutex);
  if (replaced) *replaced = s_launch_name != NULL;
  free(s_launch_name);
  s_launch_name = copy;
  uint32_t id = s_launch_next_id++;
  s_launch_id = id;
  pthread_mutex_unlock(&s_launch_mutex);
  return id;
}

char *sim_launch_take(uint32_t *launch_id) {
  pthread_mutex_lock(&s_launch_mutex);
  char *name = s_launch_name;
  s_launch_name = NULL;
  if (launch_id) *launch_id = s_launch_id;
  pthread_mutex_unlock(&s_launch_mutex);
  return name;
}

static void last_outcome_store(const char *json) {
  char *copy = json ? strdup(json) : NULL;
  pthread_mutex_lock(&s_launch_mutex);
  free(s_last_outcome);
  s_last_outcome = copy;
  pthread_mutex_unlock(&s_launch_mutex);
}

char *sim_last_outcome_json(void) {
  pthread_mutex_lock(&s_launch_mutex);
  char *copy = strdup(s_last_outcome ? s_last_outcome : "{\"launch_id\":0}");
  pthread_mutex_unlock(&s_launch_mutex);
  return copy;
}

void sim_rescan_request(void) {
  pthread_mutex_lock(&s_launch_mutex);
  s_rescan_requested = true;
  pthread_mutex_unlock(&s_launch_mutex);
}

static bool rescan_take(void) {
  pthread_mutex_lock(&s_launch_mutex);
  bool r = s_rescan_requested;
  s_rescan_requested = false;
  pthread_mutex_unlock(&s_launch_mutex);
  return r;
}

// ── App outcome ─────────────────────────────────────────────────────────────
// Written and read on the main thread only (runner → check_launch).

static struct {
  char name[64];
  char id[64];
  sim_app_result_t result;
  char error[SIM_LOG_TEXT_MAX];
  char traceback[4096];
  uint32_t start_ms;
  bool began;
} s_outcome;

static void outcome_reset(void) { memset(&s_outcome, 0, sizeof(s_outcome)); }

void sim_app_outcome_begin(const char *name, const char *id) {
  snprintf(s_outcome.name, sizeof(s_outcome.name), "%s", name ? name : "");
  snprintf(s_outcome.id, sizeof(s_outcome.id), "%s", id ? id : "");
  s_outcome.start_ms = hal_get_time_ms();
  s_outcome.began = true;
  sim_log_os("[LAUNCHER] start %s (%s)", s_outcome.name, s_outcome.id);
}

void sim_app_outcome_set(sim_app_result_t result, const char *error) {
  s_outcome.result = result;
  if (error)
    snprintf(s_outcome.error, sizeof(s_outcome.error), "%s", error);
}

void sim_app_outcome_end(bool ok) {
  // Runners that do not report (native): the launcher's bool is all we have.
  if (s_outcome.result == SIM_APP_RESULT_NONE)
    s_outcome.result = ok ? SIM_APP_RESULT_RETURNED : SIM_APP_RESULT_LOAD_FAILED;
}

void sim_app_outcome_traceback(const char *traceback) {
  snprintf(s_outcome.traceback, sizeof(s_outcome.traceback), "%s",
           traceback ? traceback : "");
}

void sim_app_report_error(const char *context, const char *message) {
  char err[SIM_LOG_TEXT_MAX];
  snprintf(err, sizeof(err), "%s %s", context ? context : "",
           message ? message : "unknown error");
  sim_app_outcome_set(SIM_APP_RESULT_ERROR, err);
  sim_log_err("%s", err);
  if (s_outcome.traceback[0]) sim_log_err("%s", s_outcome.traceback);
}

static const char *result_name(sim_app_result_t r) {
  switch (r) {
  case SIM_APP_RESULT_RETURNED: return "returned";
  case SIM_APP_RESULT_ERROR: return "error";
  case SIM_APP_RESULT_EXIT_SENTINEL: return "exit_sentinel";
  case SIM_APP_RESULT_LOAD_FAILED: return "load_failed";
  default: return "load_failed";  // runner never reported
  }
}

// app.exited params: {name,id,found,result,error,runtime_ms,launch_id}.
static char *outcome_json(const char *requested, bool found, uint32_t launch_id) {
  sim_app_result_t r = s_outcome.result;
  if (!found) {
    r = SIM_APP_RESULT_LOAD_FAILED;
    if (!s_outcome.error[0])
      snprintf(s_outcome.error, sizeof(s_outcome.error), "app not found: %s",
               requested);
  }
  uint32_t runtime = s_outcome.began ? hal_get_time_ms() - s_outcome.start_ms : 0;

  sim_strbuf_t sb = {0};
  sim_sb_append(&sb, "{\"name\":", 8);
  sim_sb_append_json_str(&sb, requested);
  sim_sb_append(&sb, ",\"id\":", 6);
  if (s_outcome.id[0]) sim_sb_append_json_str(&sb, s_outcome.id);
  else sim_sb_append(&sb, "null", 4);
  sim_sb_appendf(&sb, ",\"found\":%s,\"result\":\"%s\",\"error\":",
                 found ? "true" : "false", result_name(r));
  if (s_outcome.error[0]) sim_sb_append_json_str(&sb, s_outcome.error);
  else sim_sb_append(&sb, "null", 4);
  sim_sb_appendf(&sb, ",\"runtime_ms\":%u,\"launch_id\":%u}", runtime, launch_id);
  return sim_sb_finish(&sb);
}

// Called from the launcher loop (main thread) every iteration.
bool sim_handler_check_launch(void) {
  bool dirty = false;
  if (rescan_take()) {
    launcher_refresh_apps();
    sim_log_os("[LAUNCHER] apps rescanned");
    dirty = true;
  }

  uint32_t launch_id = 0;
  char *name = sim_launch_take(&launch_id);
  if (!name) return dirty;

  sim_strbuf_t sb = {0};
  sim_sb_append(&sb, "{\"name\":", 8);
  sim_sb_append_json_str(&sb, name);
  sim_sb_append(&sb, "}", 1);
  char *started = sim_sb_finish(&sb);
  sim_socket_notify("app.started", started ? started : "{}");
  free(started);

  outcome_reset();
  bool found = launcher_launch_by_name(name);
  if (!found) {
    // The app may have been staged after boot: rescan once and retry.
    launcher_refresh_apps();
    outcome_reset();
    found = launcher_launch_by_name(name);
  }
  char *params = outcome_json(name, found, launch_id);
  sim_log_os("[LAUNCHER] exited %s", name);
  // Stored before the notification goes out, so a client that sees the
  // notification (or times out waiting for it) always finds it here too.
  last_outcome_store(params);
  sim_socket_notify("app.exited", params ? params : "{}");
  free(params);
  free(name);
  return true;
}
