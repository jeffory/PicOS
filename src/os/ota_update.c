// ota_update.c — SD-staged OTA firmware updater
//
// Flow:
//   1. Lua app downloads .bin to /system/update.bin
//   2. Lua calls picocalc.sys.applyUpdate(path) → ota_trigger_update()
//   3. ota_trigger_update() validates the file, sets scratch[1]=OTA_MAGIC, reboots
//   4. On next boot, main() calls ota_check_pending() + ota_apply_update()
//   5. ota_apply_update() pre-reads .bin into PSRAM, then an SRAM-resident
//      function erases+programs all flash sectors and reboots.
//
// Safety:
//   - Flash writer runs BEFORE Core 1 launch (no Mongoose, no audio ISRs)
//   - Entire firmware pre-read into PSRAM before any flash writes begin
//   - Flash write loop is SRAM-resident and never calls flash-resident code
//     (the old firmware's code is overwritten sector-by-sector, so calling
//     back into flash would execute new firmware code at old addresses)
//   - Sector 0 (boot stage 2 + vector table) written last — if the write is
//     interrupted, the old firmware's boot code remains intact
//   - SRAM staging buffer used for flash_range_program (QMI bus may be
//     locked to CS0 during flash operations, making PSRAM on CS1 inaccessible)

#include "ota_update.h"

#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "../drivers/display.h"
#include "../drivers/sdcard.h"
#include "umm_malloc.h"
#include "ui.h"

// Maximum firmware size: 2MB (flash is 4MB, but leave headroom)
#define OTA_MAX_SIZE (2u * 1024u * 1024u)

// Minimum firmware size: must have at least the vector table (256 bytes)
#define OTA_MIN_SIZE 256u

// RP2350 vector table validation:
// Word 0 = initial SP (should be in SRAM: 0x20000000–0x20082000)
// Word 1 = reset vector (should be in flash: 0x10000000–0x10400000)
#define SRAM_BASE_ADDR  0x20000000u
#define SRAM_END_ADDR   0x20082000u
#define FLASH_BASE_ADDR 0x10000000u
#define FLASH_END_ADDR  0x10400000u

// SHA-256 verification via mbedTLS (already linked for HTTPS)
#include "mbedtls/sha256.h"

// ── Progress display ────────────────────────────────────────────────────────

static void ota_show_progress(uint32_t bytes_done, uint32_t bytes_total) {
    int pct = (int)((uint64_t)bytes_done * 100 / bytes_total);

    // Progress bar dimensions
    int bar_x = 40, bar_y = 170, bar_w = 240, bar_h = 16;
    int fill_w = (int)((uint64_t)bar_w * bytes_done / bytes_total);

    display_fill_rect(bar_x, bar_y, bar_w, bar_h, COLOR_DKGRAY);
    if (fill_w > 0)
        display_fill_rect(bar_x, bar_y, fill_w, bar_h, COLOR_GREEN);
    display_draw_rect(bar_x, bar_y, bar_w, bar_h, COLOR_WHITE);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d%%  (%luK / %luK)",
             pct, (unsigned long)(bytes_done / 1024),
             (unsigned long)(bytes_total / 1024));
    // Center text below progress bar
    int tw = display_text_width(buf);
    display_draw_text((320 - tw) / 2, bar_y + bar_h + 8, buf,
                      COLOR_WHITE, COLOR_BLACK);

    display_flush();
}

static void ota_show_status(const char *line1, const char *line2, uint16_t color) {
    display_clear(COLOR_BLACK);
    display_draw_text(8, 8, "PicOS Firmware Update", COLOR_CYAN, COLOR_BLACK);
    display_draw_text(8, 140, line1, color, COLOR_BLACK);
    if (line2)
        display_draw_text(8, 156, line2, COLOR_GRAY, COLOR_BLACK);
    display_flush();
}

// ── SHA-256 file verification ───────────────────────────────────────────────

// Compute SHA-256 of a file on SD card. Returns true on success.
static bool ota_sha256_file(const char *path, uint8_t out_hash[32]) {
    sdfile_t f = sdcard_fopen(path, "r");
    if (!f) return false;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 = SHA-256 (not SHA-224)

    // Use a stack buffer for hashing — SRAM only, no PSRAM
    uint8_t buf[512];
    int n;
    while ((n = sdcard_fread(f, buf, sizeof(buf))) > 0) {
        mbedtls_sha256_update(&ctx, buf, (size_t)n);
        watchdog_update();
    }

    sdcard_fclose(f);
    mbedtls_sha256_finish(&ctx, out_hash);
    mbedtls_sha256_free(&ctx);
    return true;
}

// Parse a hex SHA-256 hash file (64 hex chars). Returns true on success.
static bool ota_parse_hash_file(const char *path, uint8_t out_hash[32]) {
    sdfile_t f = sdcard_fopen(path, "r");
    if (!f) return false;

    char hex[65];
    int n = sdcard_fread(f, hex, 64);
    sdcard_fclose(f);
    if (n < 64) return false;
    hex[64] = '\0';

    for (int i = 0; i < 32; i++) {
        unsigned int byte;
        if (sscanf(&hex[i * 2], "%02x", &byte) != 1) return false;
        out_hash[i] = (uint8_t)byte;
    }
    return true;
}

// Verify .bin against .sha256 hash file. Returns true if hash matches.
static bool ota_verify_hash(const char *bin_path, const char *hash_path) {
    uint8_t file_hash[32], expected_hash[32];

    if (!ota_parse_hash_file(hash_path, expected_hash)) {
        printf("[OTA] No valid hash file at %s, skipping verification\n", hash_path);
        return true; // No hash file = skip verification (hash is optional)
    }

    ota_show_status("Verifying firmware...", "Computing SHA-256", COLOR_WHITE);

    if (!ota_sha256_file(bin_path, file_hash)) {
        printf("[OTA] Failed to read %s for hashing\n", bin_path);
        return false;
    }

    if (memcmp(file_hash, expected_hash, 32) != 0) {
        printf("[OTA] SHA-256 mismatch!\n");
        return false;
    }

    printf("[OTA] SHA-256 verified OK\n");
    return true;
}

// ── Vector table validation ─────────────────────────────────────────────────

static bool ota_validate_header(const uint8_t *data, int len) {
    if (len < 256) return false;

    printf("[OTA] Header dump: ");
    for (int i = 0; i < 32 && i < len; i++) {
        printf("%02x ", data[i]);
    }
    printf("\n");

    if (memcmp(data, "UF2\n", 4) == 0) {
        printf("[OTA] Error: File is UF2 format, not .bin!\n");
        return false;
    }

    uint32_t sp, reset_vec;
    memcpy(&sp, data, 4);
    memcpy(&reset_vec, data + 4, 4);

    if (sp < SRAM_BASE_ADDR || sp > SRAM_END_ADDR) {
        printf("[OTA] Invalid SP: 0x%08lx (expected 0x20000000-0x20082000)\n", (unsigned long)sp);
        return false;
    }

    uint32_t rv = reset_vec & ~1u;
    if (rv < FLASH_BASE_ADDR || rv >= FLASH_END_ADDR) {
        printf("[OTA] Invalid reset vector: 0x%08lx (expected 0x10000000-0x10400000)\n", (unsigned long)reset_vec);
        return false;
    }

    return true;
}

// ── SRAM-resident flash writer ──────────────────────────────────────────────
// This function runs entirely from SRAM and NEVER calls back into flash.
// Once flash writes begin, the old firmware's code is progressively
// overwritten — any call to a flash-resident function would execute the NEW
// firmware's code at the OLD addresses, causing undefined behaviour.
//
// The function writes sectors 1..N first (deferring sector 0 so the old
// boot code stays intact as long as possible), then writes sector 0 last,
// and reboots without returning to flash.
//
// fw_data:  firmware image in PSRAM (QMI CS1, accessible between flash ops)
// fw_size:  total firmware size in bytes
//
// This function does NOT return.

static void __no_inline_not_in_flash_func(ota_write_and_reboot)(
    const uint8_t *fw_data, uint32_t fw_size) {

    // SRAM staging buffer — flash_range_program reads from this during
    // erase/program when the QMI bus is locked to CS0 (flash), so the
    // source must be in SRAM, not PSRAM.
    static uint8_t staging[FLASH_SECTOR_SIZE];

    // Write sectors 1..N (defer sector 0)
    for (uint32_t off = FLASH_SECTOR_SIZE; off < fw_size;
         off += FLASH_SECTOR_SIZE) {
        uint32_t chunk = fw_size - off;
        if (chunk > FLASH_SECTOR_SIZE) chunk = FLASH_SECTOR_SIZE;

        // Copy PSRAM → SRAM staging (byte loop — no flash-resident memcpy)
        for (uint32_t i = 0; i < chunk; i++)
            staging[i] = fw_data[off + i];
        for (uint32_t i = chunk; i < FLASH_SECTOR_SIZE; i++)
            staging[i] = 0xFF; // pad to full sector

        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(off, FLASH_SECTOR_SIZE);
        flash_range_program(off, staging, FLASH_SECTOR_SIZE);
        restore_interrupts(ints);

        // Feed watchdog directly (watchdog_update is in flash)
        watchdog_hw->load = 10u * 1000u * 1000u; // ~10 s reload
    }

    // Commit sector 0 (boot stage 2 + vector table) — last write
    {
        uint32_t chunk = fw_size < FLASH_SECTOR_SIZE ? fw_size : FLASH_SECTOR_SIZE;
        for (uint32_t i = 0; i < chunk; i++)
            staging[i] = fw_data[i];
        for (uint32_t i = chunk; i < FLASH_SECTOR_SIZE; i++)
            staging[i] = 0xFF;

        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(0, FLASH_SECTOR_SIZE);
        flash_range_program(0, staging, FLASH_SECTOR_SIZE);
        restore_interrupts(ints);
    }

    // Clear scratch registers and reboot into new firmware.
    // Cannot call watchdog_reboot() (flash-resident) — use direct reset.
    watchdog_hw->scratch[0] = 0; // clear boot counter
    watchdog_hw->scratch[OTA_SCRATCH_IDX] = 0; // clear OTA flag

    // ARM System Reset via AIRCR register (direct write — no flash code).
    // PPB_BASE + M33_AIRCR_OFFSET = 0xE000ED0C (Application Interrupt and
    // Reset Control Register).  VECTKEY=0x05FA, SYSRESETREQ=bit 2.
    volatile uint32_t *aircr = (volatile uint32_t *)(PPB_BASE + M33_AIRCR_OFFSET);
    __dmb(); // data memory barrier
    *aircr = (0x05FAu << 16) | (1u << 2);
    __dmb();
    while (1) { /* wait for reset */ }
}

// ── Public API ──────────────────────────────────────────────────────────────

bool ota_check_pending(void) {
    return watchdog_hw->scratch[OTA_SCRATCH_IDX] == OTA_MAGIC;
}

bool ota_apply_update(void) {
    printf("[OTA] Applying firmware update from %s\n", OTA_BIN_PATH);

    // Verify hash if hash file exists
    if (!ota_verify_hash(OTA_BIN_PATH, OTA_HASH_PATH)) {
        ota_show_status("Update failed!", "SHA-256 hash mismatch", COLOR_RED);
        printf("[OTA] Hash verification failed\n");
        goto fail;
    }

    // Open the firmware file
    int file_size = sdcard_fsize(OTA_BIN_PATH);
    if (file_size <= 0 || (uint32_t)file_size < OTA_MIN_SIZE) {
        ota_show_status("Update failed!", "Firmware file too small", COLOR_RED);
        printf("[OTA] File too small: %d bytes\n", file_size);
        goto fail;
    }
    if ((uint32_t)file_size > OTA_MAX_SIZE) {
        ota_show_status("Update failed!", "Firmware file too large", COLOR_RED);
        printf("[OTA] File too large: %d bytes\n", file_size);
        goto fail;
    }

    // ── Pre-read entire firmware into PSRAM ─────────────────────────────────
    // The flash write loop is SRAM-resident and cannot call flash-resident
    // code (SD card, FatFS, display, etc.) because those flash regions get
    // overwritten during the update.  Pre-reading into PSRAM means the write
    // loop only needs PSRAM reads and SRAM staging — no SD card access.
    ota_show_status("Loading firmware...", "Reading from SD card", COLOR_WHITE);
    watchdog_update();

    uint8_t *fw_buf = (uint8_t *)umm_malloc((uint32_t)file_size);
    if (!fw_buf) {
        ota_show_status("Update failed!", "Not enough memory", COLOR_RED);
        printf("[OTA] umm_malloc(%d) failed\n", file_size);
        goto fail;
    }

    sdfile_t f = sdcard_fopen(OTA_BIN_PATH, "r");
    if (!f) {
        ota_show_status("Update failed!", "Cannot open firmware file", COLOR_RED);
        printf("[OTA] Cannot open %s\n", OTA_BIN_PATH);
        umm_free(fw_buf);
        goto fail;
    }

    // Read in chunks with progress
    uint32_t total = (uint32_t)file_size;
    uint32_t bytes_read = 0;
    while (bytes_read < total) {
        int n = sdcard_fread(f, fw_buf + bytes_read, FLASH_SECTOR_SIZE);
        if (n <= 0) break;
        bytes_read += (uint32_t)n;
        ota_show_progress(bytes_read, total);
        watchdog_update();
    }
    sdcard_fclose(f);

    if (bytes_read < total) {
        ota_show_status("Update failed!", "SD card read error", COLOR_RED);
        printf("[OTA] Read only %lu of %lu bytes\n",
               (unsigned long)bytes_read, (unsigned long)total);
        umm_free(fw_buf);
        goto fail;
    }

    // Validate vector table from the pre-read buffer
    if (!ota_validate_header(fw_buf, (int)total)) {
        ota_show_status("Update failed!", "Invalid firmware header", COLOR_RED);
        umm_free(fw_buf);
        goto fail;
    }

    printf("[OTA] Firmware loaded into PSRAM: %lu bytes\n", (unsigned long)total);

    // ── Rename update files BEFORE flash writes ─────────────────────────────
    // After flash writes begin, SD card functions (flash-resident) can't be
    // called.  Rename now so the file doesn't re-trigger on next boot.
    sdcard_delete(OTA_BIN_PATH ".flashed");
    sdcard_delete(OTA_HASH_PATH ".flashed");
    if (!sdcard_rename(OTA_BIN_PATH, OTA_BIN_PATH ".flashed")) {
        printf("[OTA] Rename failed, deleting %s\n", OTA_BIN_PATH);
        sdcard_delete(OTA_BIN_PATH);
    }
    if (!sdcard_rename(OTA_HASH_PATH, OTA_HASH_PATH ".flashed")) {
        sdcard_delete(OTA_HASH_PATH);
    }

    // ── Write firmware to flash ─────────────────────────────────────────────
    // Point of no return — this function does not return.  It writes all
    // flash sectors from PSRAM (SRAM-resident, no flash code called),
    // clears scratch registers, and reboots into the new firmware.
    ota_show_status("Writing firmware...", "DO NOT POWER OFF!", COLOR_YELLOW);
    sleep_ms(500);
    watchdog_update();
    stdio_flush();

    printf("[OTA] Starting flash write (%lu bytes)...\n", (unsigned long)total);
    stdio_flush();

    // This call never returns
    ota_write_and_reboot(fw_buf, total);

    // Unreachable
    while (1) tight_loop_contents();

fail:
    // Clear the OTA flag so we don't loop on failed updates
    watchdog_hw->scratch[OTA_SCRATCH_IDX] = 0;
    return false;
}

bool ota_trigger_update(const char *bin_path, const char **out_err) {
    int size = sdcard_fsize(bin_path);
    printf("[OTA] Triggering update from %s, size=%d\n", bin_path, size);
    if (size < 0) {
        *out_err = "Firmware file not found";
        return false;
    }
    if ((uint32_t)size < OTA_MIN_SIZE) {
        *out_err = "Firmware file too small";
        return false;
    }
    if ((uint32_t)size > OTA_MAX_SIZE) {
        *out_err = "Firmware file too large (max 2MB)";
        return false;
    }

    sdfile_t f = sdcard_fopen(bin_path, "r");
    if (!f) {
        *out_err = "Cannot open firmware file";
        return false;
    }
    uint8_t header[256];
    int n = sdcard_fread(f, header, sizeof(header));
    sdcard_fclose(f);

    if (n < (int)sizeof(header)) {
        *out_err = "Cannot read firmware header";
        return false;
    }

    if (!ota_validate_header(header, n)) {
        *out_err = "Invalid firmware (bad vector table)";
        return false;
    }

    // If the file is not already at the standard path, it needs to be there
    // for the boot-time updater to find it.
    if (strcmp(bin_path, OTA_BIN_PATH) != 0) {
        // Copy to standard location
        if (!sdcard_copy(bin_path, OTA_BIN_PATH, NULL, NULL)) {
            *out_err = "Failed to copy firmware to /system/update.bin";
            return false;
        }
    }

    printf("[OTA] Triggering update: %d bytes, rebooting...\n", size);
    stdio_flush();

    // Set OTA magic and reboot
    watchdog_hw->scratch[OTA_SCRATCH_IDX] = OTA_MAGIC;
    sleep_ms(100);
    watchdog_reboot(0, 0, 0);

    // Unreachable
    while (1) tight_loop_contents();
    return true;
}
