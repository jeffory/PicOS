// pico/mutex.h stub for simulator

#ifndef PICO_MUTEX_H
#define PICO_MUTEX_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

// mutex_t wraps pthread_mutex_t
typedef struct {
    pthread_mutex_t mtx;
} mutex_t;

// Recursive mutex (for compatibility)
typedef struct {
    pthread_mutex_t mtx;
} recursive_mutex_t;

static inline void mutex_init(mutex_t* m) {
    pthread_mutex_init(&m->mtx, NULL);
}

static inline void mutex_deinit(mutex_t* m) {
    pthread_mutex_destroy(&m->mtx);
}

static inline void mutex_enter_blocking(mutex_t* m) {
    pthread_mutex_lock(&m->mtx);
}

static inline void mutex_exit(mutex_t* m) {
    pthread_mutex_unlock(&m->mtx);
}

static inline bool mutex_try_enter(mutex_t* m, uint32_t* owner_out) {
    (void)owner_out;
    return pthread_mutex_trylock(&m->mtx) == 0;
}

// Recursive mutex functions
static inline void recursive_mutex_init(recursive_mutex_t* m) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->mtx, &attr);
    pthread_mutexattr_destroy(&attr);
}

static inline void recursive_mutex_deinit(recursive_mutex_t* m) {
    pthread_mutex_destroy(&m->mtx);
}

static inline void recursive_mutex_enter_blocking(recursive_mutex_t* m) {
    pthread_mutex_lock(&m->mtx);
}

static inline void recursive_mutex_exit(recursive_mutex_t* m) {
    pthread_mutex_unlock(&m->mtx);
}

static inline bool recursive_mutex_try_enter(recursive_mutex_t* m, uint32_t* owner_out) {
    (void)owner_out;
    return pthread_mutex_trylock(&m->mtx) == 0;
}

#endif // PICO_MUTEX_H
