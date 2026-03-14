// OPL register capture — writes .opcl binary to SD card
// Compile with -DOPL_CAPTURE to enable

#ifdef OPL_CAPTURE

#include "opl_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// .opcl binary format:
//   Header (16 bytes):
//     "OPCL"           4 bytes magic
//     uint16_t ver     version (1)
//     uint16_t rate    sample rate (11025)
//     uint16_t mus_hz  MUS tick rate (140)
//     6 bytes reserved
//
//   Records (4 bytes each):
//     0x01 reg val 0x00   register write
//     0x02 count 0x00 0x00  advance N samples (1-255)
//     0xFF 0x00 0x00 0x00   end of stream

#define OPCL_MAGIC      "OPCL"
#define OPCL_VERSION    1
#define OPCL_RATE       11025
#define OPCL_MUS_HZ     140

#define CAPTURE_BUF_RECORDS  8192
#define CAPTURE_BUF_BYTES    (CAPTURE_BUF_RECORDS * 4)
#define CAPTURE_FLUSH_THRESH (CAPTURE_BUF_RECORDS * 3 / 4)  // flush at 75%

// Max samples to capture (~30s at 11025Hz)
#define CAPTURE_MAX_SAMPLES  (11025 * 30)

extern char g_app_dir[];

static struct {
    uint8_t *buf;
    int      count;      // records in buffer
    FILE    *fp;
    int      active;
    int      total_samples;
} s_cap;

static void capture_flush(void)
{
    if (!s_cap.fp || s_cap.count == 0)
        return;
    fwrite(s_cap.buf, 4, s_cap.count, s_cap.fp);
    s_cap.count = 0;
}

static void capture_emit(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    if (!s_cap.active)
        return;

    if (s_cap.count >= CAPTURE_BUF_RECORDS) {
        capture_flush();
        if (s_cap.count >= CAPTURE_BUF_RECORDS)
            return;  // flush failed
    }

    int off = s_cap.count * 4;
    s_cap.buf[off + 0] = a;
    s_cap.buf[off + 1] = b;
    s_cap.buf[off + 2] = c;
    s_cap.buf[off + 3] = d;
    s_cap.count++;

    if (s_cap.count >= CAPTURE_FLUSH_THRESH)
        capture_flush();
}

void opl_capture_init(void)
{
    memset(&s_cap, 0, sizeof(s_cap));

    s_cap.buf = malloc(CAPTURE_BUF_BYTES);
    if (!s_cap.buf) {
        printf("[OPL_CAP] malloc failed\n");
        return;
    }

    char path[160];
    snprintf(path, sizeof(path), "%s/opl_dump.opcl", g_app_dir);
    s_cap.fp = fopen(path, "wb");
    if (!s_cap.fp) {
        printf("[OPL_CAP] can't open %s\n", path);
        free(s_cap.buf);
        s_cap.buf = NULL;
        return;
    }

    // Write header
    uint8_t hdr[16];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, OPCL_MAGIC, 4);
    hdr[4] = OPCL_VERSION & 0xFF;
    hdr[5] = (OPCL_VERSION >> 8) & 0xFF;
    hdr[6] = OPCL_RATE & 0xFF;
    hdr[7] = (OPCL_RATE >> 8) & 0xFF;
    hdr[8] = OPCL_MUS_HZ & 0xFF;
    hdr[9] = (OPCL_MUS_HZ >> 8) & 0xFF;
    fwrite(hdr, 1, 16, s_cap.fp);

    s_cap.active = 1;
    printf("[OPL_CAP] capture started -> %s\n", path);
}

void opl_capture_reg_write(uint8_t reg, uint8_t val)
{
    if (!s_cap.active)
        return;
    capture_emit(0x01, reg, val, 0x00);
}

void opl_capture_advance(int sample_count)
{
    if (!s_cap.active)
        return;

    s_cap.total_samples += sample_count;
    if (s_cap.total_samples >= CAPTURE_MAX_SAMPLES) {
        opl_capture_stop();
        return;
    }

    // Emit advance records (max 255 samples each)
    while (sample_count > 0) {
        int n = sample_count > 255 ? 255 : sample_count;
        capture_emit(0x02, (uint8_t)n, 0x00, 0x00);
        sample_count -= n;
    }
}

void opl_capture_stop(void)
{
    if (!s_cap.active)
        return;

    s_cap.active = 0;

    // Write end-of-stream marker
    if (s_cap.fp) {
        // Flush remaining records
        capture_flush();
        // End marker
        uint8_t end[4] = { 0xFF, 0x00, 0x00, 0x00 };
        fwrite(end, 1, 4, s_cap.fp);
        fclose(s_cap.fp);
        s_cap.fp = NULL;
    }

    if (s_cap.buf) {
        free(s_cap.buf);
        s_cap.buf = NULL;
    }

    printf("[OPL_CAP] capture stopped (%d samples)\n", s_cap.total_samples);
}

#endif // OPL_CAPTURE
