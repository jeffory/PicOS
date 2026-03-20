// HAL Threading - POSIX Implementation

#include "hal_threading.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

bool hal_thread_create(thread_t* thread, void* (*func)(void*), void* arg) {
    pthread_t* pth = (pthread_t*)malloc(sizeof(pthread_t));
    if (!pth) return false;
    
    if (pthread_create(pth, NULL, func, arg) != 0) {
        free(pth);
        return false;
    }
    
    thread->handle = pth;
    return true;
}

void hal_thread_join(thread_t* thread) {
    if (thread && thread->handle) {
        pthread_join(*(pthread_t*)thread->handle, NULL);
        free(thread->handle);
        thread->handle = NULL;
    }
}

// Mutex functions now use pico/mutex.h types directly
