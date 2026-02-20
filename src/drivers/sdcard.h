#pragma once

#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// SD Card / Filesystem Driver
// Uses Chan FatFS (bundled in pico-extras or vendored in third_party/fatfs).
// SPI0 at 10 MHz: MISO=GP16, CS=GP17, SCK=GP18, MOSI=GP19
//
// All paths are relative to the SD card root ("/").
// App directories live at:  /apps/<app_name>/main.lua
//                            /apps/<app_name>/app.json
// Shared config lives at:   /system/config.json
// App save data lives at:   /data/<app_name>/<file>
// =============================================================================

// Returns true if the SD card was found and mounted successfully
bool sdcard_init(void);

// Returns true if an SD card is currently mounted
bool sdcard_is_mounted(void);

// Remount (e.g. after card swap). Returns true on success.
bool sdcard_remount(void);

// ── File I/O ─────────────────────────────────────────────────────────────────

typedef void *sdfile_t;  // opaque FIL* handle

sdfile_t sdcard_fopen(const char *path, const char *mode);
int      sdcard_fread(sdfile_t f, void *buf, int len);
int      sdcard_fwrite(sdfile_t f, const void *buf, int len);
void     sdcard_fclose(sdfile_t f);
bool     sdcard_fexists(const char *path);
int      sdcard_fsize(const char *path);   // Returns -1 on error
bool     sdcard_fseek(sdfile_t f, uint32_t offset);
uint32_t sdcard_ftell(sdfile_t f);

// Create a directory (and parent directories if needed)
bool     sdcard_mkdir(const char *path);

// ── Directory listing ─────────────────────────────────────────────────────────

typedef struct {
    char name[64];
    bool is_dir;
    uint32_t size;
} sdcard_entry_t;

// Calls callback for each entry in path. Returns count of entries, -1 on error.
int sdcard_list_dir(const char *path,
                    void (*callback)(const sdcard_entry_t *entry, void *user),
                    void *user);

// ── Convenience: read entire file into a heap-allocated buffer ─────────────-
// Caller must free() the returned pointer. Returns NULL on error.
// *out_len is set to the file size.
char *sdcard_read_file(const char *path, int *out_len);
