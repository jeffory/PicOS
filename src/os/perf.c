#include "perf.h"
#include "pico/stdlib.h"
#include "hardware/structs/xip.h"
#include <stdio.h>
#include <string.h>

#define PERF_SAMPLES 30

static uint32_t s_perf_frame_times[PERF_SAMPLES] = {0};
static int s_perf_index = 0;
static uint32_t s_perf_frame_start = 0;
static uint32_t s_perf_last_frame_time = 0;
static int s_perf_fps = 0;
static uint32_t s_perf_target_fps = 0;
static uint32_t s_perf_target_frame_ms = 0;

void perf_init(void) {
    s_perf_frame_start = 0;
    s_perf_index = 0;
    s_perf_fps = 0;
    s_perf_last_frame_time = 0;
    s_perf_target_fps = 0;
    s_perf_target_frame_ms = 0;
    memset(s_perf_frame_times, 0, sizeof(s_perf_frame_times));
}

void perf_begin_frame(void) {
    if (s_perf_frame_start == 0) {
        s_perf_frame_start = to_ms_since_boot(get_absolute_time());
    }
}

void perf_end_frame(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (s_perf_frame_start != 0) {
        uint32_t delta = now - s_perf_frame_start;

        s_perf_last_frame_time = delta;
        s_perf_frame_times[s_perf_index] = delta;
        s_perf_index = (s_perf_index + 1) % PERF_SAMPLES;

        uint32_t sum = 0;
        int count = 0;
        for (int i = 0; i < PERF_SAMPLES; i++) {
            if (s_perf_frame_times[i] > 0) {
                sum += s_perf_frame_times[i];
                count++;
            }
        }
        uint32_t avg_frame_time = (count > 0) ? (sum / count) : 0;
        s_perf_fps = (avg_frame_time > 0) ? (1000 / avg_frame_time) : 0;
    }

    if (s_perf_target_fps > 0) {
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - s_perf_frame_start;
        if (elapsed < s_perf_target_frame_ms) {
            sleep_ms(s_perf_target_frame_ms - elapsed);
        }
    }

    s_perf_frame_start = to_ms_since_boot(get_absolute_time());
}

int perf_get_fps(void) {
    return s_perf_fps;
}

uint32_t perf_get_frame_time(void) {
    return s_perf_last_frame_time;
}

void perf_set_target_fps(uint32_t fps) {
    s_perf_target_fps = fps;
    s_perf_target_frame_ms = (fps > 0) ? (1000 / fps) : 0;
}

// ── XIP cache performance counters ──────────────────────────────────────────
// RP2350 XIP controller has two counters:
//   ctr_acc — total cache accesses (reads from XIP address space)
//   ctr_hit — cache hits (served from cache without flash/PSRAM fetch)
// Both are 32-bit and auto-reset on read of ctr_acc.

void perf_xip_cache_reset(void) {
    // Reading ctr_acc clears both counters (datasheet §10.4)
    (void)xip_ctrl_hw->ctr_acc;
}

int perf_xip_cache_hit_rate(void) {
    uint32_t hits = xip_ctrl_hw->ctr_hit;
    uint32_t total = xip_ctrl_hw->ctr_acc;  // clears both
    if (total == 0) return -1;
    return (int)((uint64_t)hits * 100 / total);
}

void perf_xip_cache_report(void) {
    uint32_t hits = xip_ctrl_hw->ctr_hit;
    uint32_t total = xip_ctrl_hw->ctr_acc;  // clears both
    if (total == 0) {
        printf("[XIP] No cache accesses recorded\n");
        return;
    }
    int rate = (int)((uint64_t)hits * 100 / total);
    printf("[XIP] Cache: %lu accesses, %lu hits (%d%% hit rate)\n",
           (unsigned long)total, (unsigned long)hits, rate);
}
