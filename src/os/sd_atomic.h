#pragma once

// =============================================================================
// Atomic replace of a small SD file (config.c, appconfig.c)
//
// Writing a store in place (fopen "w" truncates) loses it to a power cut or a
// failed write mid-save — for /system/config.json that is the WiFi
// credentials. Instead:
//   1. write <path>.tmp and close it (sdcard_fclose syncs);
//   2. move the current <path> aside to <path>.bak (FatFS f_rename refuses to
//      replace an existing file, so the target must be out of the way);
//   3. rename <path>.tmp -> <path>; on failure move <path>.bak back;
//   4. delete <path>.bak.
// Any failure returns false with <path> holding the old contents and no .tmp
// left behind. The only window without <path> is between 2 and 3, and then
// <path>.bak is the old file: sd_atomic_recover (call before loading) moves
// it back.
// =============================================================================

#include "../drivers/sdcard.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define SD_ATOMIC_PATH_MAX 192

static inline bool sd_atomic_names(const char *path, char *tmp, char *bak) {
  size_t n = strlen(path);
  if (n + 5 > SD_ATOMIC_PATH_MAX)
    return false;
  memcpy(tmp, path, n);
  memcpy(tmp + n, ".tmp", 5);
  memcpy(bak, path, n);
  memcpy(bak + n, ".bak", 5);
  return true;
}

// Before loading: a save interrupted between moving the old file aside and
// moving the new one in left only <path>.bak; put it back.
static inline void sd_atomic_recover(const char *path) {
  char tmp[SD_ATOMIC_PATH_MAX], bak[SD_ATOMIC_PATH_MAX];
  if (!sd_atomic_names(path, tmp, bak))
    return;
  if (!sdcard_fexists(path) && sdcard_fexists(bak)) {
    if (sdcard_rename(bak, path))
      printf("[SD] %s: restored from an interrupted save\n", path);
  }
}

static inline bool sd_atomic_write(const char *path, const char *buf, int len) {
  char tmp[SD_ATOMIC_PATH_MAX], bak[SD_ATOMIC_PATH_MAX];
  if (!sd_atomic_names(path, tmp, bak)) {
    printf("[SD] %s: path too long for an atomic save\n", path);
    return false;
  }
  sdfile_t f = sdcard_fopen(tmp, "w");
  if (!f) {
    printf("[SD] %s: cannot open for writing\n", tmp);
    return false;
  }
  int written = sdcard_fwrite(f, buf, len);
  sdcard_fclose(f);
  if (written != len) {
    printf("[SD] %s: write truncated (%d/%d)\n", tmp, written, len);
    sdcard_delete(tmp);
    return false;
  }
  bool had_old = sdcard_fexists(path);
  if (had_old) {
    if (sdcard_fexists(bak))
      sdcard_delete(bak);  // left by an earlier failed cleanup
    if (!sdcard_rename(path, bak)) {
      printf("[SD] %s: cannot move the old file aside\n", path);
      sdcard_delete(tmp);
      return false;
    }
  }
  if (!sdcard_rename(tmp, path)) {
    printf("[SD] %s: cannot move the new file in\n", path);
    if (had_old && !sdcard_rename(bak, path))
      printf("[SD] %s: old file left as %s\n", path, bak);
    sdcard_delete(tmp);
    return false;
  }
  if (had_old)
    sdcard_delete(bak);
  return true;
}
