// HAL PSRAM - Host Memory Implementation

#ifndef HAL_PSRAM_H
#define HAL_PSRAM_H

#include <stdbool.h>
#include <stddef.h>

// Initialize PSRAM subsystem
bool hal_psram_init(void);

// Shutdown PSRAM subsystem
void hal_psram_shutdown(void);

// Allocate memory from PSRAM
void* hal_psram_malloc(size_t size);

// Free PSRAM memory
void hal_psram_free(void* ptr);

// Reallocate PSRAM memory
void* hal_psram_realloc(void* ptr, size_t size);

// Get total PSRAM size
size_t hal_psram_total_size(void);

// Get free PSRAM size
size_t hal_psram_free_size(void);

#endif // HAL_PSRAM_H
