#pragma once

// The PCM stream ring between producers (audio_push_samples: fileplayer,
// modplayer, the native/Lua stream API) and the DMA refill ISR's mixer
// (audio.c).  Header-only and always inlined: the pop runs per output frame
// inside the __time_critical_func mixer, which must not call out to flash.
// Host-tested in tests/unit/test_audio_ring.c.
//
// Single producer, single consumer: write is only advanced by the producer,
// read only by the consumer.  Indices are free-running uint32 counters (the
// used count is write - read, correct across wrap); the slot is idx & MASK.
// Samples are stored as unsigned 8-bit per channel.

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_RING_SIZE 4096u  // frames; must be a power of 2
#define AUDIO_RING_MASK (AUDIO_RING_SIZE - 1u)

typedef struct {
    uint8_t l[AUDIO_RING_SIZE];
    uint8_t r[AUDIO_RING_SIZE];
    volatile uint32_t write;  // producer
    volatile uint32_t read;   // consumer
    uint32_t phase;           // consumer's rate-conversion accumulator
} audio_ring_t;

#define AUDIO_RING_INLINE static inline __attribute__((always_inline))

AUDIO_RING_INLINE void audio_ring_clear(audio_ring_t *r) {
    r->read = 0;
    r->write = 0;
}

AUDIO_RING_INLINE uint32_t audio_ring_used(const audio_ring_t *r) {
    return r->write - r->read;
}

// Free frames the producer may push.
AUDIO_RING_INLINE uint32_t audio_ring_space(const audio_ring_t *r) {
    uint32_t used = r->write - r->read;
    if (used > AUDIO_RING_SIZE)
        return 0;  // shouldn't happen
    return AUDIO_RING_SIZE - used;
}

// Push up to count interleaved stereo int16 frames; frames that do not fit
// are dropped.  Returns the number pushed.
AUDIO_RING_INLINE int audio_ring_push(audio_ring_t *r, const int16_t *samples,
                                      int count) {
    int i = 0;
    for (; i < count; i++) {
        uint32_t avail = r->write - r->read;
        if (avail >= AUDIO_RING_SIZE)
            break;  // ring full, drop remaining samples

        int16_t lv = samples[i * 2 + 0];
        int16_t rv = samples[i * 2 + 1];

        uint32_t idx = r->write & AUDIO_RING_MASK;
        // int16_t [-32768,32767] -> uint8_t [0,255]
        r->l[idx] = (uint8_t)((lv + 32768) >> 8);
        r->r[idx] = (uint8_t)((rv + 32768) >> 8);
        r->write++;
    }
    return i;
}

// One output frame at out_rate from content recorded at content_rate
// (nearest-neighbour: 11025 Hz content emits each frame 4x at 44100).
// Returns false on underrun (ring empty: *ml = *mr = 0, nothing moves).
AUDIO_RING_INLINE bool audio_ring_pop(audio_ring_t *r, uint32_t content_rate,
                                      uint32_t out_rate, int32_t *ml,
                                      int32_t *mr) {
    uint32_t w = r->write;
    uint32_t rd = r->read;
    if (rd == w) {
        *ml = 0;
        *mr = 0;
        return false;
    }
    uint32_t idx = rd & AUDIO_RING_MASK;
    // uint8 [0,255] -> centered int16 (* 256, not << 8: shifting a negative
    // value left is undefined; compilers emit the same shift)
    *ml = ((int32_t)r->l[idx] - 128) * 256;
    *mr = ((int32_t)r->r[idx] - 128) * 256;
    // Advance the source at the content's own rate.
    r->phase += content_rate;
    while (r->phase >= out_rate) {
        r->phase -= out_rate;
        if (r->read != r->write)
            r->read++;
    }
    return true;
}
