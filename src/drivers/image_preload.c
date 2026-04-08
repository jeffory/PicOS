#include "image_preload.h"
#include "image_api.h"
#include <string.h>
#include <stdatomic.h>
#include "pico/platform.h"
#include "pico/multicore.h"
#include "wifi.h"  // for WIFI_IPC_DOORBELL

#define PRELOAD_PATH_MAX 256

typedef enum {
    PRELOAD_IDLE = 0,
    PRELOAD_REQUESTED,
    PRELOAD_DECODING,
    PRELOAD_DONE,
    PRELOAD_FAILED,
    PRELOAD_CANCELLED
} preload_state_t;

static _Atomic int s_state = PRELOAD_IDLE;
static char        s_path[PRELOAD_PATH_MAX];  // Core 0 writes, Core 1 reads
static pc_image_t *s_result = NULL;           // Core 1 writes, Core 0 reads

void image_preload_init(void) {
    atomic_store(&s_state, PRELOAD_IDLE);
    s_result = NULL;
}

bool image_preload_start(const char *path) {
    if (!path) return false;

    int cur = atomic_load(&s_state);

    // If currently decoding, mark cancelled — caller should retry next frame
    if (cur == PRELOAD_DECODING) {
        atomic_store(&s_state, PRELOAD_CANCELLED);
        return false;
    }

    // Free any unclaimed result from a previous preload
    if (cur == PRELOAD_DONE || cur == PRELOAD_FAILED) {
        if (s_result) {
            image_free(s_result);
            s_result = NULL;
        }
    }

    strncpy(s_path, path, PRELOAD_PATH_MAX - 1);
    s_path[PRELOAD_PATH_MAX - 1] = '\0';
    s_result = NULL;
    __dmb();  // path visible before state change
    atomic_store(&s_state, PRELOAD_REQUESTED);

    // Wake Core 1 immediately
    multicore_doorbell_set_other_core(WIFI_IPC_DOORBELL);
    return true;
}

pc_image_t *image_preload_poll(bool *ready) {
    int cur = atomic_load(&s_state);

    if (cur == PRELOAD_DONE) {
        pc_image_t *img = s_result;
        s_result = NULL;
        atomic_store(&s_state, PRELOAD_IDLE);
        if (ready) *ready = true;
        return img;
    }

    if (cur == PRELOAD_FAILED) {
        atomic_store(&s_state, PRELOAD_IDLE);
        if (ready) *ready = true;
        return NULL;
    }

    if (ready) *ready = false;
    return NULL;
}

void image_preload_cancel(void) {
    int cur = atomic_load(&s_state);

    if (cur == PRELOAD_DECODING) {
        // Core 1 will check this after decode and free the result
        atomic_store(&s_state, PRELOAD_CANCELLED);
    } else {
        if (s_result) {
            image_free(s_result);
            s_result = NULL;
        }
        atomic_store(&s_state, PRELOAD_IDLE);
    }
}

// Called on Core 1 each tick
void image_preload_update(void) {
    int cur = atomic_load(&s_state);
    if (cur != PRELOAD_REQUESTED) return;

    atomic_store(&s_state, PRELOAD_DECODING);

    // Copy path locally in case Core 0 modifies s_path (shouldn't while DECODING)
    char path[PRELOAD_PATH_MAX];
    memcpy(path, s_path, PRELOAD_PATH_MAX);

    pc_image_t *img = image_load(path);

    // Check if cancelled during decode
    if (atomic_load(&s_state) == PRELOAD_CANCELLED) {
        if (img) image_free(img);
        atomic_store(&s_state, PRELOAD_IDLE);
        return;
    }

    s_result = img;
    __dmb();  // result visible before state change
    atomic_store(&s_state, img ? PRELOAD_DONE : PRELOAD_FAILED);
}
