// HAL Threading - Cross-platform thread implementation

#ifndef HAL_THREADING_H
#define HAL_THREADING_H

#include <stdbool.h>
#include "pico/mutex.h"

// Thread handle
typedef struct {
    void* handle;
} thread_t;

// Create a new thread
bool hal_thread_create(thread_t* thread, void* (*func)(void*), void* arg);

// Wait for thread to finish
void hal_thread_join(thread_t* thread);

// Mutex functions are in pico/mutex.h

#endif // HAL_THREADING_H
