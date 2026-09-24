#pragma once

// Simulator test-control hooks called from shared OS code.
//
// The simulator's control channel (simulator/sim_test_control.c) needs to
// know how an app ended and see error text that firmware only puts on screen
// and in /system/error.log. The OS calls these hooks at those points. On
// firmware every hook is an empty static inline, so the calls compile away.

#include <stdbool.h>
#include <stdint.h>

// How an app run ended; reported in the simulator's app.exited notification.
typedef enum {
  SIM_APP_RESULT_NONE = 0,       // runner did not report (native apps)
  SIM_APP_RESULT_RETURNED,       // main chunk returned normally
  SIM_APP_RESULT_ERROR,          // runtime error (error screen shown)
  SIM_APP_RESULT_EXIT_SENTINEL,  // sys.exit / system-menu exit / exit_app
  SIM_APP_RESULT_LOAD_FAILED,    // never ran: refused, missing, parse error
} sim_app_result_t;

#ifdef PICOS_SIMULATOR

// Launcher is about to run an app.
void sim_app_outcome_begin(const char *name, const char *id);
// Record the result. `error` (may be NULL) replaces the stored error text
// only when non-NULL, so a later LOAD_FAILED keeps an earlier error message.
void sim_app_outcome_set(sim_app_result_t result, const char *error);
// Launcher: the runner returned `ok`. Fills in the result when the runner
// did not record one (native apps).
void sim_app_outcome_end(bool ok);
// Lua error message handler output (traceback) for the error being raised.
void sim_app_outcome_traceback(const char *traceback);
// A Lua error screen is being shown: log context, message and the stored
// traceback to the "err" log source and record SIM_APP_RESULT_ERROR.
void sim_app_report_error(const char *context, const char *message);
// Append a formatted line to the "err" / "os" log source.
void sim_log_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sim_log_os(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// --test-mode: error screens return at once and idle dimming is off.
bool sim_test_mode(void);

#else

static inline void sim_app_outcome_begin(const char *name, const char *id) {
  (void)name;
  (void)id;
}
static inline void sim_app_outcome_set(sim_app_result_t result,
                                       const char *error) {
  (void)result;
  (void)error;
}
static inline void sim_app_outcome_end(bool ok) { (void)ok; }
static inline void sim_app_outcome_traceback(const char *traceback) {
  (void)traceback;
}
static inline void sim_app_report_error(const char *context,
                                        const char *message) {
  (void)context;
  (void)message;
}
static inline void sim_log_err(const char *fmt, ...) { (void)fmt; }
static inline void sim_log_os(const char *fmt, ...) { (void)fmt; }
static inline bool sim_test_mode(void) { return false; }

#endif
