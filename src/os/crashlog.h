#ifndef CRASHLOG_H
#define CRASHLOG_H

// Append a Lua error to /system/error.log.  Safe to call from normal context
// (uses sdcard_fopen/fwrite/fclose).
void crashlog_write_lua_error(const char *app_name, const char *context,
                              const char *message);

#endif // CRASHLOG_H
