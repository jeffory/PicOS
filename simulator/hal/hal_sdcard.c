// HAL SD Card - Host Filesystem Implementation
// Maps SD card operations to host filesystem

#include "hal_sdcard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

char g_base_path[512] = ".";
static int g_initialized = 0;

bool hal_sdcard_init(const char* base_path) {
    if (base_path) {
        strncpy(g_base_path, base_path, sizeof(g_base_path) - 1);
        g_base_path[sizeof(g_base_path) - 1] = '\0';
    }
    
    // Create standard directories if they don't exist
    char path[1024];
    
    snprintf(path, sizeof(path), "%s/apps", g_base_path);
    mkdir(path, 0755);
    
    snprintf(path, sizeof(path), "%s/data", g_base_path);
    mkdir(path, 0755);
    
    snprintf(path, sizeof(path), "%s/system", g_base_path);
    mkdir(path, 0755);
    
    g_initialized = 1;
    printf("[SDCard] Initialized with base path: %s\n", g_base_path);
    return true;
}

void hal_sdcard_shutdown(void) {
    g_initialized = 0;
    printf("[SDCard] Shutdown\n");
}

// Build full path from relative path
static void build_path(char* out, size_t out_size, const char* path) {
    if (path[0] == '/') {
        // Absolute path - prepend base
        snprintf(out, out_size, "%s%s", g_base_path, path);
    } else {
        // Relative path
        snprintf(out, out_size, "%s/%s", g_base_path, path);
    }
}

// Simulate FatFS file operations
void* hal_sdcard_open(const char* path, const char* mode) {
    if (!g_initialized) return NULL;
    
    char full_path[1024];
    build_path(full_path, sizeof(full_path), path);
    
    return fopen(full_path, mode);
}

void hal_sdcard_close(void* handle) {
    if (handle) {
        fclose((FILE*)handle);
    }
}

size_t hal_sdcard_read(void* handle, void* buf, size_t len) {
    if (!handle) return 0;
    return fread(buf, 1, len, (FILE*)handle);
}

size_t hal_sdcard_write(void* handle, const void* buf, size_t len) {
    if (!handle) return 0;
    return fwrite(buf, 1, len, (FILE*)handle);
}

int hal_sdcard_seek(void* handle, long offset) {
    if (!handle) return -1;
    return fseek((FILE*)handle, offset, SEEK_SET);
}

long hal_sdcard_tell(void* handle) {
    if (!handle) return -1;
    return ftell((FILE*)handle);
}

int hal_sdcard_exists(const char* path) {
    if (!g_initialized) return 0;
    
    char full_path[1024];
    build_path(full_path, sizeof(full_path), path);
    
    struct stat st;
    return stat(full_path, &st) == 0;
}

int hal_sdcard_size(const char* path) {
    if (!g_initialized) return -1;
    
    char full_path[1024];
    build_path(full_path, sizeof(full_path), path);
    
    struct stat st;
    if (stat(full_path, &st) != 0) return -1;
    return (int)st.st_size;
}

int hal_sdcard_mkdir(const char* path) {
    if (!g_initialized) return -1;
    
    char full_path[1024];
    build_path(full_path, sizeof(full_path), path);
    
    return mkdir(full_path, 0755);
}
