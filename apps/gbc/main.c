#include "app_abi.h"
#include "os.h"
#include "display.h"
#include "input.h"
#include "fs.h"
#include <stddef.h>
#include "minigb_apu.h"
#include "peanut_gb.h"
#include <string.h>
#include <stdio.h>

// ROM_BANK_SIZE (0x4000) is defined by peanut_gb.h — do not redefine it here.
// ROM_MAX_SIZE: 1 MB covers all common DMG/CGB games (Kirby=256KB, Pokemon=1MB).
// Larger games (>1MB) will load bank 0 correctly but return 0xFF for missing banks.
#define ROM_MAX_SIZE  (1024 * 1024)
#define CART_RAM_SIZE 0x8000

static uint8_t s_rom[ROM_MAX_SIZE];
static int     s_rom_size = 0;
static uint8_t s_ram[CART_RAM_SIZE];
// AUDIO_SAMPLES (~738) involves float division and isn't a constant expression,
// so use a fixed-size buffer.  748 > 738 gives slight headroom per frame.
// 4 frames per display flush × stereo (2 channels) × int16_t.
#define GBC_AUDIO_BUF_FRAMES 748
static int16_t s_audio_buf[GBC_AUDIO_BUF_FRAMES * 2 * 4];

static struct gb_s s_gb;
static GBCDisplay s_display;
static GBCInput s_input;
static GBCFilesystem s_fs;
static const PicoCalcAPI *s_api;

static uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr) {
    (void)gb;
    if (addr < (uint_fast32_t)s_rom_size) {
        return s_rom[addr];
    }
    return 0xFF;
}

static uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
    (void)gb;
    if (addr < CART_RAM_SIZE) {
        return s_ram[addr];
    }
    return 0xFF;
}

static void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, uint8_t val) {
    (void)gb;
    if (addr < CART_RAM_SIZE) {
        s_ram[addr] = val;
    }
}

static void gb_error(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr) {
    (void)gb;
    (void)addr;
    s_api->sys->log("GB Error: %d at 0x%04X\n", err, addr);
}

static void lcd_draw_line(struct gb_s *gb, const uint8_t pixels[160], const uint_fast8_t line) {
    gbc_display_draw_line(&s_display, pixels, line, gb);
}

static char g_rom_list[16][32];
static int g_rom_count = 0;

static void rom_list_callback(const char *name, bool is_dir, uint32_t size, void *user) {
    (void)user;
    (void)size;
    s_api->sys->log("[GBC] rom_list_cb: '%s' dir=%d\n", name, is_dir);
    if (is_dir) return;
    
    const char *ext = name;
    while (*ext) ext++;
    while (ext > name && ext[-1] != '.') ext--;
    
    if (g_rom_count < 16 && (ext[0] == 'g' && ext[1] == 'b')) {
        int len = 0;
        while (name[len] && len < 31) {
            g_rom_list[g_rom_count][len] = name[len];
            len++;
        }
        g_rom_list[g_rom_count][len] = '\0';
        g_rom_count++;
    }
}

static char *find_first_rom(void) {
    g_rom_count = 0;

    s_api->sys->log("[GBC] find_first_rom: checking exists...\n");
    if (!s_api->fs->exists("/data/com.picos.gbc/roms")) {
        s_api->sys->log("[GBC] find_first_rom: roms dir does not exist\n");
        return NULL;
    }
    s_api->sys->log("[GBC] find_first_rom: roms dir exists, listing...\n");

    s_api->fs->listDir("/data/com.picos.gbc/roms", rom_list_callback, NULL);
    s_api->sys->log("[GBC] find_first_rom: listDir returned, count=%d\n", g_rom_count);

    if (g_rom_count > 0) {
        static char result[64];
        int i = 0;
        const char *prefix = "/data/com.picos.gbc/roms/";
        while (prefix[i]) {
            result[i] = prefix[i];
            i++;
        }
        int j = 0;
        while (g_rom_list[0][j]) {
            result[i + j] = g_rom_list[0][j];
            j++;
        }
        result[i + j] = '\0';
        return result;
    }
    return NULL;
}

void picos_main(const PicoCalcAPI *api,
                const char *app_dir,
                const char *app_id,
                const char *app_name)
{
    (void)app_dir;
    (void)app_id;
    (void)app_name;
    
    s_api = api;

    const picocalc_display_t *d = api->display;
    const picocalc_sys_t *sys = api->sys;
    const picocalc_input_t *in = api->input;
    sys->log("[GBC] picos_main entered\n");
    d->clear(0x0000);
    d->drawText(100, 150, "GBC...", 0xFFFF, 0x0000);
    d->flush();
    sys->log("[GBC] splash drawn\n");

    gbc_display_init(&s_display);
    sys->log("[GBC] display_init done\n");
    gbc_input_init(&s_input);
    sys->log("[GBC] input_init done\n");
    gbc_fs_init(&s_fs);
    sys->log("[GBC] fs_init done\n");
    gbc_fs_set_api(api);
    sys->log("[GBC] fs_set_api done\n");

    sys->log("[GBC] calling find_first_rom()...\n");
    char *rom_path = find_first_rom();
    sys->log("[GBC] find_first_rom() returned: %s\n", rom_path ? rom_path : "(null)");
    if (!rom_path) {
        d->clear(0x0000);
        d->drawText(60, 150, "No ROMs found!", 0xF800, 0x0000);
        d->drawText(40, 170, "Put .gb files in", 0xFFFF, 0x0000);
        d->drawText(40, 185, "/data/com.picos.gbc/roms/", 0xFFFF, 0x0000);
        d->flush();
        
        while (1) {
            sys->poll();
            if ((in->getButtonsPressed() & BTN_ESC) || sys->shouldExit()) {
                return;
            }
        }
    }
    
    d->clear(0x0000);
    d->drawText(80, 150, "Loading ROM...", 0x07E0, 0x0000);
    d->flush();
    sys->log("[GBC] Loading ROM screen drawn\n");

    sys->log("[GBC] loading ROM: %s\n", rom_path);
    sys->poll(); // feed watchdog before potentially long SD read
    s_rom_size = gbc_fs_load_rom(&s_fs, rom_path, s_rom, ROM_MAX_SIZE);
    sys->poll(); // feed watchdog after ROM load
    sys->log("[GBC] ROM load complete: path=%s size=%d\n", rom_path, s_rom_size);
    if (s_rom_size <= 0) {
        d->clear(0x0000);
        d->drawText(60, 150, "Failed to load ROM", 0xF800, 0x0000);
        d->flush();
        
        while (1) {
            sys->poll();
            if ((in->getButtonsPressed() & BTN_ESC) || sys->shouldExit()) {
                return;
            }
        }
    }
    
    d->clear(0x0000);
    d->drawText(80, 150, "Init GB...", 0x07E0, 0x0000);
    d->flush();
    sys->log("[GBC] Init GB screen drawn\n");

    sys->log("[GBC] calling gb_init...\n");
    enum gb_init_error_e ret = gb_init(&s_gb, &gb_rom_read, &gb_cart_ram_read,
                                        &gb_cart_ram_write, &gb_error, NULL);
    sys->log("[GBC] gb_init returned: %d\n", (int)ret);
    if (ret != GB_INIT_NO_ERROR) {
        sys->log("GB init failed: error %d\n", (int)ret);
        d->clear(0x0000);
        d->drawText(60, 150, "GB Init Failed", 0xF800, 0x0000);
        d->flush();
        
        while (1) {
            sys->poll();
            if ((in->getButtonsPressed() & BTN_ESC) || sys->shouldExit()) {
                return;
            }
        }
    }
    
    // Wire direct ROM/RAM pointers for inlined access in __gb_read/__gb_write
    s_gb.rom = s_rom;
    s_gb.rom_size = s_rom_size;
    s_gb.cart_ram_data = s_ram;
    s_gb.cart_ram_data_size = CART_RAM_SIZE;

    gb_init_lcd(&s_gb, &lcd_draw_line);
    s_gb.direct.frame_skip = 1;
    s_display.cgb_mode = s_gb.cgb.cgbMode;
    s_display.cgb_palette = s_gb.cgb.fixPalette;
    gbc_display_update_cgb_lut(&s_display);
    sys->log("[GBC] lcd callback set (cgb=%d)\n", s_display.cgb_mode);

    audio_init();
    api->audio->startStream(AUDIO_SAMPLE_RATE);
    sys->log("[GBC] audio started at %u Hz\n", AUDIO_SAMPLE_RATE);

    uint32_t save_size = gb_get_save_size(&s_gb);
    sys->log("[GBC] save_size=%lu\n", (unsigned long)save_size);
    if (save_size > 0 && save_size <= CART_RAM_SIZE) {
        gbc_fs_load_ram(&s_fs, s_ram, save_size);
        sys->log("[GBC] save RAM loaded\n");
    }

    // Clear entire screen once before entering the game loop.
    // The GBC image (320x288) sits at y=16, leaving 16px top + 16px bottom black.
    d->clear(0x0000);
    d->flush();
    sys->log("[GBC] entering main loop\n");

    // FPS counter — updates once per second in the 16px top bar
    uint32_t fps_last_time = sys->getTimeMs();
    int fps_frame_count = 0;
    char fps_str[16] = "FPS: --";

    // PIO SPI runs at ~100MHz, so DMA flush takes ~17ms for a full 320x320
    // frame (~58fps max).  We emulate 4 GB frames between each display flush
    // to keep the game running at native ~60fps speed.  Input is polled
    // between every GB frame so button latency stays <20ms.
    bool running = true;
    while (running) {
        for (int f = 0; f < 4 && running; f++) {
            sys->poll();
            gbc_input_update(&s_input, in->getButtons);
            s_gb.direct.joypad_bits.a      = (s_input.buttons & BTN_F4)    ? 0 : 1;
            s_gb.direct.joypad_bits.b      = (s_input.buttons & BTN_F5)    ? 0 : 1;
            s_gb.direct.joypad_bits.select = (s_input.buttons & BTN_F1)    ? 0 : 1;
            s_gb.direct.joypad_bits.start  = (s_input.buttons & BTN_F2)    ? 0 : 1;
            s_gb.direct.joypad_bits.right  = (s_input.buttons & BTN_RIGHT) ? 0 : 1;
            s_gb.direct.joypad_bits.left   = (s_input.buttons & BTN_LEFT)  ? 0 : 1;
            s_gb.direct.joypad_bits.up     = (s_input.buttons & BTN_UP)    ? 0 : 1;
            s_gb.direct.joypad_bits.down   = (s_input.buttons & BTN_DOWN)  ? 0 : 1;

            if (in->getButtonsPressed() & BTN_ESC) {
                running = false;
                break;
            }
            if (sys->shouldExit()) {
                running = false;
                break;
            }

            s_gb.gb_frame = 0;
            while (s_gb.gb_frame == 0) {
                __gb_step_cpu(&s_gb);
            }

            audio_callback(NULL,
                           s_audio_buf + f * AUDIO_SAMPLES * 2,
                           AUDIO_BUFFER_SIZE_BYTES);
        }

        if (running) {
            api->audio->pushSamples(s_audio_buf, AUDIO_SAMPLES * 4);
        }

        if (running) {
            // FPS counter — update once per second
            fps_frame_count++;
            uint32_t now = sys->getTimeMs();
            uint32_t elapsed = now - fps_last_time;
            if (elapsed >= 1000) {
                int fps = (fps_frame_count * 1000) / elapsed;
                // Simple itoa into fps_str
                fps_str[0]='F'; fps_str[1]='P'; fps_str[2]='S';
                fps_str[3]=':'; fps_str[4]=' ';
                if (fps >= 100) {
                    fps_str[5] = '0' + (fps / 100);
                    fps_str[6] = '0' + ((fps / 10) % 10);
                    fps_str[7] = '0' + (fps % 10);
                    fps_str[8] = '\0';
                } else if (fps >= 10) {
                    fps_str[5] = '0' + (fps / 10);
                    fps_str[6] = '0' + (fps % 10);
                    fps_str[7] = '\0';
                } else {
                    fps_str[5] = '0' + fps;
                    fps_str[6] = '\0';
                }
                fps_frame_count = 0;
                fps_last_time = now;
            }
            // Draw in the 16px black bar above the GBC image
            d->drawText(2, 4, fps_str, 0x07E0, 0x0000);

            // Refresh CGB palette LUT (palettes can change mid-game)
            if (s_display.cgb_mode)
                gbc_display_update_cgb_lut(&s_display);

            gbc_display_render(&s_display,
                d->drawImageNN,
                d->flush,
                d->flushRows);
        }
    }

    api->audio->stopStream();
}
