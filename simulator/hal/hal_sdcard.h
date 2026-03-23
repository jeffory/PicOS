// HAL SD Card - Host Filesystem Implementation

#ifndef HAL_SDCARD_H
#define HAL_SDCARD_H

#include <stdbool.h>
#include <stddef.h>

// Initialize SD card subsystem with base path
bool hal_sdcard_init(const char* base_path);

// Shutdown SD card subsystem
void hal_sdcard_shutdown(void);

// File operations (similar to FatFS API)
void* hal_sdcard_open(const char* path, const char* mode);
void hal_sdcard_close(void* handle);
size_t hal_sdcard_read(void* handle, void* buf, size_t len);
size_t hal_sdcard_write(void* handle, const void* buf, size_t len);
int hal_sdcard_seek(void* handle, long offset);
long hal_sdcard_tell(void* handle);

// File/directory info
int hal_sdcard_exists(const char* path);
int hal_sdcard_size(const char* path);
int hal_sdcard_mkdir(const char* path);

#endif // HAL_SDCARD_H
