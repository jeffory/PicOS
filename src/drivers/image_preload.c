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

    // If currently decoding, request cancellation — caller should retry next frame.
    // CAS so we don't clobber a DONE/FAILED that Core 1 wrote between the load and store.
    if (cur == PRELOAD_DECODING) {
        if (atomic_compare_exchange_strong(&s_state, &cur, PRELOAD_CANCELLED)) {
            return false;
        }
        // CAS failed: cur now holds the actual state (DONE/FAILED), fall through.
    }

    // Cancel in flight — Core 1 hasn't observed it yet. Caller retries next frame.
    if (cur == PRELOAD_CANCELLED) {
        return false;
    }

    // Reclaim previous result before starting a new request. CAS ensures
    // we own s_result before freeing (no concurrent writer at this point,
    // but the CAS keeps the state machine honest).
    if (cur == PRELOAD_DONE || cur == PRELOAD_FAILED) {
        if (!atomic_compare_exchange_strong(&s_state, &cur, PRELOAD_IDLE)) {
            return false;  // lost race — caller retries
        }
        if (s_result) {
            image_free(s_result);
            s_result = NULL;
        }
        cur = PRELOAD_IDLE;
    }

    if (cur != PRELOAD_IDLE) {
        return false;  // REQUESTED or other unexpected state — caller retries
    }

    strncpy(s_path, path, PRELOAD_PATH_MAX - 1);
    s_path[PRELOAD_PATH_MAX - 1] = '\0';
    s_result = NULL;
    __dmb();  // path visible before state change
    if (!atomic_compare_exchange_strong(&s_state, &cur, PRELOAD_REQUESTED)) {
        return false;  // lost race — caller retries
    }

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
    while (1) {
        int cur = atomic_load(&s_state);
        switch (cur) {
        case PRELOAD_IDLE:
        case PRELOAD_CANCELLED:
            return;  // already settled or cancellation already in flight

        case PRELOAD_DECODING:
            // CAS so we don't clobber a DONE/FAILED Core 1 wrote in the meantime.
            // On success Core 1 sees CANCELLED post-decode and frees the result.
            if (atomic_compare_exchange_strong(&s_state, &cur, PRELOAD_CANCELLED)) {
                return;
            }
            break;  // retry — cur was updated

        case PRELOAD_REQUESTED:
            // Core 1 hasn't picked it up yet — drop straight to IDLE.
            if (atomic_compare_exchange_strong(&s_state, &cur, PRELOAD_IDLE)) {
                return;
            }
            break;  // retry

        case PRELOAD_DONE:
        case PRELOAD_FAILED:
            // Free any unclaimed result and reset.
            if (atomic_compare_exchange_strong(&s_state, &cur, PRELOAD_IDLE)) {
                if (s_result) {
                    image_free(s_result);
                    s_result = NULL;
                }
                return;
            }
            break;  // retry
        }
    }
}

// Called on Core 1 each tick
void image_preload_update(void) {
    // CAS REQUESTED -> DECODING so a concurrent cancel() that flips REQUESTED
    // straight to IDLE wins cleanly instead of being overwritten.
    int expected = PRELOAD_REQUESTED;
    if (!atomic_compare_exchange_strong(&s_state, &expected, PRELOAD_DECODING)) {
        return;
    }

    // Copy path locally in case Core 0 starts a new request after we transition.
    char path[PRELOAD_PATH_MAX];
    memcpy(path, s_path, PRELOAD_PATH_MAX);

    pc_image_t *img = image_load(path);

    // Publish the result before the state flip; poll() reads s_result only
    // when it observes DONE/FAILED.
    s_result = img;
    __dmb();

    // CAS DECODING -> DONE/FAILED. If Core 0 cancelled mid-decode the state
    // is now CANCELLED and the CAS fails — clean up the orphan image.
    expected = PRELOAD_DECODING;
    int new_state = img ? PRELOAD_DONE : PRELOAD_FAILED;
    if (!atomic_compare_exchange_strong(&s_state, &expected, new_state)) {
        s_result = NULL;
        if (img) image_free(img);
        atomic_store(&s_state, PRELOAD_IDLE);
    }
}
