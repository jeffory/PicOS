#include "clock.h"
#include "config.h"

#include "pico/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── State ─────────────────────────────────────────────────────────────────────

static bool     s_synced       = false;
static uint32_t s_epoch_base   = 0;   // UTC epoch at last sync
static uint32_t s_pico_base_ms = 0;   // to_ms_since_boot() value at last sync

// ── Public API ─────────────────────────────────────────────────────────────────

void clock_sntp_set(unsigned sec) {
    s_epoch_base   = (uint32_t)sec;
    s_pico_base_ms = to_ms_since_boot(get_absolute_time());
    s_synced       = true;
    printf("Clock: NTP sync → epoch=%u\n", sec);
}

bool clock_is_set(void) {
    return s_synced;
}

uint32_t clock_get_epoch(void) {
    if (!s_synced) return 0;
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    // Unsigned subtraction handles ~49-day ms-counter rollover correctly.
    uint32_t elapsed_s = (now_ms - s_pico_base_ms) / 1000u;
    return s_epoch_base + elapsed_s;
}

bool clock_get_time(int *h, int *m, int *s) {
    if (!s_synced) return false;

    uint32_t epoch = clock_get_epoch();

    // Apply tz_offset from config (integer minutes, may be negative).
    // e.g. "330" = UTC+5:30, "-300" = UTC-5.
    int tz_min = 0;
    const char *tz_str = config_get("tz_offset");
    if (tz_str && tz_str[0]) tz_min = (int)strtol(tz_str, NULL, 10);

    // Adjust epoch by tz offset (signed arithmetic; cast to int32 is safe for
    // reasonable UTC offsets and epoch values in the next few decades).
    int32_t local = (int32_t)epoch + tz_min * 60;
    if (local < 0) local = 0;

    uint32_t day_sec = (uint32_t)local % 86400u;
    *h = (int)(day_sec / 3600u);
    *m = (int)((day_sec % 3600u) / 60u);
    *s = (int)(day_sec % 60u);
    return true;
}

bool clock_format(char *buf, int len) {
    int h, m, s;
    if (!clock_get_time(&h, &m, &s)) {
        if (len >= 6) {
            buf[0] = '-'; buf[1] = '-'; buf[2] = ':';
            buf[3] = '-'; buf[4] = '-'; buf[5] = '\0';
        }
        return false;
    }
    snprintf(buf, (size_t)len, "%02d:%02d", h, m);
    return true;
}
