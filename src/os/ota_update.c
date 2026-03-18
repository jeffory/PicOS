// ota_update.c — SD-staged OTA firmware updater
//
// Flow:
//   1. Lua app downloads .bin to /system/update.bin
//   2. Lua calls picocalc.sys.applyUpdate(path) → ota_trigger_update()
//   3. ota_trigger_update() validates the file, sets scratch[0]=OTA_MAGIC, reboots
//   4. On next boot, main() calls ota_check_pending() + ota_apply_update()
//   5. ota_apply_update() reads .bin from SD, erases+programs flash, reboots
//
// Safety:
//   - Flash writer runs BEFORE Core 1 launch (no Mongoose, no audio ISRs)
//   - SD SPI0 and display PIO0 are independent of flash XIP
//   - All buffers are in SRAM (stack or static), not PSRAM

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

    uint32_t sp, reset_vec;
    memcpy(&sp, data, 4);
    memcpy(&reset_vec, data + 4, 4);

    // Initial SP should be in SRAM range
    if (sp < SRAM_BASE_ADDR || sp > SRAM_END_ADDR) {
        printf("[OTA] Invalid SP: 0x%08lx\n", (unsigned long)sp);
        return false;
    }

    // Reset vector should be in flash range (strip Thumb bit)
    uint32_t rv = reset_vec & ~1u;
    if (rv < FLASH_BASE_ADDR || rv >= FLASH_END_ADDR) {
        printf("[OTA] Invalid reset vector: 0x%08lx\n", (unsigned long)reset_vec);
        return false;
    }

    return true;
}

// ── Flash writer (SRAM-resident) ────────────────────────────────────────────
// This function MUST execute from SRAM because flash is unavailable during
// erase/program operations. The Pico SDK macro ensures correct placement.

static void __no_inline_not_in_flash_func(ota_flash_sector)(
    uint32_t offset, const uint8_t *data, uint32_t len) {
    // Disable interrupts — no ISRs can run while flash is being written.
    // Core 1 is not started yet, so no multicore_lockout needed.
    uint32_t ints = save_and_disable_interrupts();

    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_range_program(offset, data, len);

    restore_interrupts(ints);
}

// ── Public API ──────────────────────────────────────────────────────────────

bool ota_check_pending(void) {
    return watchdog_hw->scratch[0] == OTA_MAGIC;
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

    sdfile_t f = sdcard_fopen(OTA_BIN_PATH, "r");
    if (!f) {
        ota_show_status("Update failed!", "Cannot open firmware file", COLOR_RED);
        printf("[OTA] Cannot open %s\n", OTA_BIN_PATH);
        goto fail;
    }

    // Read and validate the first sector (contains vector table)
    uint8_t sector_buf[FLASH_SECTOR_SIZE]; // 4KB on stack — fits in main stack
    int n = sdcard_fread(f, sector_buf, FLASH_SECTOR_SIZE);
    if (n < (int)OTA_MIN_SIZE) {
        ota_show_status("Update failed!", "Cannot read firmware header", COLOR_RED);
        sdcard_fclose(f);
        goto fail;
    }

    if (!ota_validate_header(sector_buf, n)) {
        ota_show_status("Update failed!", "Invalid firmware header", COLOR_RED);
        sdcard_fclose(f);
        goto fail;
    }

    ota_show_status("Writing firmware...", "DO NOT POWER OFF!", COLOR_YELLOW);
    sleep_ms(500); // Brief pause so user can read the warning
    watchdog_update();

    // ── Flash erase + program loop ──────────────────────────────────────────
    // Process the first sector we already read, then continue reading.
    uint32_t offset = 0;
    uint32_t total = (uint32_t)file_size;
    bool first_sector = true;

    // Pad the last read to a full sector (flash_range_program needs page-aligned)
    if (n < (int)FLASH_SECTOR_SIZE)
        memset(sector_buf + n, 0xFF, FLASH_SECTOR_SIZE - (uint32_t)n);

    while (offset < total) {
        if (!first_sector) {
            n = sdcard_fread(f, sector_buf, FLASH_SECTOR_SIZE);
            if (n <= 0) break;
            // Pad partial last sector with 0xFF (erased flash value)
            if (n < (int)FLASH_SECTOR_SIZE)
                memset(sector_buf + n, 0xFF, FLASH_SECTOR_SIZE - (uint32_t)n);
        }
        first_sector = false;

        // Write this sector to flash
        ota_flash_sector(offset, sector_buf, FLASH_SECTOR_SIZE);

        offset += FLASH_SECTOR_SIZE;

        // Update progress display and watchdog
        ota_show_progress(offset < total ? offset : total, total);
        watchdog_update();
    }

    sdcard_fclose(f);

    // Verify we wrote the expected amount
    if (offset < total) {
        ota_show_status("Update failed!", "Incomplete write - DO NOT REBOOT", COLOR_RED);
        printf("[OTA] Incomplete write: %lu of %lu bytes\n",
               (unsigned long)offset, (unsigned long)total);
        // Don't delete the .bin — user can retry
        goto fail;
    }

    printf("[OTA] Flash write complete: %lu bytes\n", (unsigned long)total);

    // Clean up: delete the update files
    ota_show_status("Update complete!", "Rebooting...", COLOR_GREEN);
    sdcard_delete(OTA_BIN_PATH);
    sdcard_delete(OTA_HASH_PATH);

    // Clear OTA flag and reboot into new firmware
    watchdog_hw->scratch[0] = 0;
    sleep_ms(500);
    watchdog_reboot(0, 0, 0);

    // Unreachable
    while (1) tight_loop_contents();
    return true;

fail:
    // Clear the OTA flag so we don't loop on failed updates
    watchdog_hw->scratch[0] = 0;
    return false;
}

bool ota_trigger_update(const char *bin_path, const char **out_err) {
    // Validate file exists and has reasonable size
    int size = sdcard_fsize(bin_path);
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

    // Read and validate the header (first 256 bytes)
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
    watchdog_hw->scratch[0] = OTA_MAGIC;
    sleep_ms(100);
    watchdog_reboot(0, 0, 0);

    // Unreachable
    while (1) tight_loop_contents();
    return true;
}
