// HAL PSRAM - Host Memory Implementation

#include "hal_psram.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

// Simulate 8MB of PSRAM
#define PSRAM_SIM_SIZE (8 * 1024 * 1024)

static size_t g_used = 0;
static int g_initialized = 0;

typedef struct {
    size_t size;
} psram_hdr_t;

bool hal_psram_init(void) {
    g_initialized = 1;
    g_used = 0;
    printf("[PSRAM] Initialized (%d MB)\n", PSRAM_SIM_SIZE / (1024 * 1024));
    return true;
}

void hal_psram_shutdown(void) {
    g_initialized = 0;
    printf("[PSRAM] Shutdown\n");
}

void* hal_psram_malloc(size_t size) {
    if (!g_initialized) return NULL;
    
    psram_hdr_t* hdr = malloc(sizeof(psram_hdr_t) + size);
    if (!hdr) return NULL;
    hdr->size = size;
    g_used += size;
    return (char*)hdr + sizeof(psram_hdr_t);
}

void hal_psram_free(void* ptr) {
    if (ptr) {
        psram_hdr_t* hdr = (psram_hdr_t*)((char*)ptr - sizeof(psram_hdr_t));
        g_used -= hdr->size;
        free(hdr);
    }
}

void* hal_psram_realloc(void* ptr, size_t size) {
    if (!g_initialized) return NULL;
    if (!ptr) return hal_psram_malloc(size);
    
    psram_hdr_t* hdr = (psram_hdr_t*)((char*)ptr - sizeof(psram_hdr_t));
    size_t old_size = hdr->size;
    psram_hdr_t* new_hdr = realloc(hdr, sizeof(psram_hdr_t) + size);
    if (!new_hdr) return NULL;
    g_used -= old_size;
    g_used += size;
    new_hdr->size = size;
    return (char*)new_hdr + sizeof(psram_hdr_t);
}

size_t hal_psram_total_size(void) {
    return PSRAM_SIM_SIZE;
}

size_t hal_psram_free_size(void) {
    // Just return a large value - on PC we have plenty of RAM
    return PSRAM_SIM_SIZE - g_used;
}
