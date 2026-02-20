#include "sdcard.h"
#include "../hardware.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

// FatFS headers — these come from the pico-extras FatFS port
// or your vendored third_party/fatfs directory.
// See CMakeLists.txt for the include path configuration.
#include "ff.h"
#include "diskio.h"

#include <stdlib.h>
#include <string.h>

// ── FatFS globals ─────────────────────────────────────────────────────────────

static FATFS s_fs;
static bool  s_mounted = false;

// ── SPI SD card low-level (bit-bang layer for FatFS diskio) ──────────────────
// FatFS calls the diskio_ functions. Those in turn call these SPI helpers.
// The actual diskio.c implementation is in third_party/fatfs/port/diskio.c
// which you configure to use SPI0 with the pins below.
// These are exposed here so diskio.c can reference the hardware configuration.

const uint s_sd_spi_baud = SD_SPI_BAUD;
const uint s_sd_pin_cs   = SD_PIN_CS;
const uint s_sd_pin_miso = SD_PIN_MISO;
const uint s_sd_pin_mosi = SD_PIN_MOSI;
const uint s_sd_pin_sck  = SD_PIN_SCK;

bool sdcard_init(void) {
    // SPI0 hardware setup
    spi_init(SD_SPI_PORT, 400 * 1000);   // 400 kHz for card init
    spi_set_format(SD_SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_set_function(SD_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MISO, GPIO_FUNC_SPI);
    gpio_pull_up(SD_PIN_MISO);   // Hold MISO high when card tristates the line

    gpio_init(SD_PIN_CS);
    gpio_set_dir(SD_PIN_CS, GPIO_OUT);
    gpio_put(SD_PIN_CS, 1);   // deselect

    // Give the card ~80 clock cycles to power up
    sleep_ms(10);

    FRESULT res = f_mount(&s_fs, "", 1);
    if (res == FR_OK) {
        s_mounted = true;

        // Ensure standard directory structure exists
        f_mkdir("/apps");
        f_mkdir("/data");
        f_mkdir("/system");
    }

    return s_mounted;
}

bool sdcard_is_mounted(void) {
    return s_mounted;
}

bool sdcard_remount(void) {
    f_unmount("");
    s_mounted = false;
    FRESULT res = f_mount(&s_fs, "", 1);
    s_mounted = (res == FR_OK);
    return s_mounted;
}

// ── FatFS mode string → FATFS flags ──────────────────────────────────────────

static BYTE mode_to_fatfs(const char *mode) {
    BYTE m = 0;
    bool has_r = false, has_w = false, has_a = false;
    for (const char *p = mode; *p; p++) {
        if (*p == 'r') has_r = true;
        if (*p == 'w') has_w = true;
        if (*p == 'a') has_a = true;
    }
    if (has_r && !has_w) m = FA_READ | FA_OPEN_EXISTING;
    if (has_w)           m = FA_WRITE | FA_CREATE_ALWAYS;
    if (has_a)           m = FA_WRITE | FA_OPEN_APPEND | FA_OPEN_ALWAYS;
    if (has_r && has_w)  m = FA_READ | FA_WRITE | FA_OPEN_ALWAYS;
    return m;
}

// ── File I/O ─────────────────────────────────────────────────────────────────

sdfile_t sdcard_fopen(const char *path, const char *mode) {
    if (!s_mounted) return NULL;
    FIL *f = (FIL *)malloc(sizeof(FIL));
    if (!f) return NULL;

    FRESULT res = f_open(f, path, mode_to_fatfs(mode));
    if (res != FR_OK) { free(f); return NULL; }
    return (sdfile_t)f;
}

int sdcard_fread(sdfile_t f, void *buf, int len) {
    if (!f) return -1;
    UINT br = 0;
    FRESULT res = f_read((FIL *)f, buf, (UINT)len, &br);
    return (res == FR_OK) ? (int)br : -1;
}

int sdcard_fwrite(sdfile_t f, const void *buf, int len) {
    if (!f) return -1;
    UINT bw = 0;
    FRESULT res = f_write((FIL *)f, buf, (UINT)len, &bw);
    return (res == FR_OK) ? (int)bw : -1;
}

void sdcard_fclose(sdfile_t f) {
    if (!f) return;
    f_close((FIL *)f);
    free(f);
}

bool sdcard_fseek(sdfile_t f, uint32_t offset) {
    if (!f) return false;
    return f_lseek((FIL *)f, (FSIZE_t)offset) == FR_OK;
}

uint32_t sdcard_ftell(sdfile_t f) {
    if (!f) return 0;
    return (uint32_t)f_tell((FIL *)f);
}

bool sdcard_fexists(const char *path) {
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK;
}

int sdcard_fsize(const char *path) {
    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK) return -1;
    return (int)fi.fsize;
}

bool sdcard_mkdir(const char *path) {
    if (!s_mounted) return false;
    // f_mkdir returns FR_OK if successful or FR_EXIST if already exists
    FRESULT res = f_mkdir(path);
    return (res == FR_OK || res == FR_EXIST);
}

int sdcard_list_dir(const char *path,
                    void (*callback)(const sdcard_entry_t *entry, void *user),
                    void *user) {
    DIR dir;
    FILINFO fi;
    if (f_opendir(&dir, path) != FR_OK) return -1;

    int count = 0;
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        sdcard_entry_t e;
        strncpy(e.name, fi.fname, sizeof(e.name) - 1);
        e.name[sizeof(e.name) - 1] = '\0';
        e.is_dir = (fi.fattrib & AM_DIR) != 0;
        e.size   = fi.fsize;
        if (callback) callback(&e, user);
        count++;
    }
    f_closedir(&dir);
    return count;
}

char *sdcard_read_file(const char *path, int *out_len) {
    int size = sdcard_fsize(path);
    if (size < 0) return NULL;

    char *buf = (char *)malloc(size + 1);
    if (!buf) return NULL;

    sdfile_t f = sdcard_fopen(path, "rb");
    if (!f) { free(buf); return NULL; }

    int n = sdcard_fread(f, buf, size);
    sdcard_fclose(f);

    if (n < 0) { free(buf); return NULL; }
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}
