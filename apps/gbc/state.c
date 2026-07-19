#include "state.h"
#include "os.h"
#include <string.h>

#define STATE_DIR     "/data/com.picos.gbc/states/"
#define STATE_VERSION 1u

typedef struct {
    char     magic[4];       // "GBST"
    uint32_t version;        // STATE_VERSION
    uint32_t gb_size;        // sizeof(struct gb_s) of the saving build
    uint32_t cart_ram_size;
    uint32_t rom_size;
    uint16_t rom_checksum;   // cart global checksum, rom[0x14E]<<8 | rom[0x14F]
    uint16_t reserved;
} gbc_state_header_t;

static void build_state_path(char *out, int out_len, const char *rom_name) {
    out[0] = '\0';
    strncat(out, STATE_DIR, (size_t)out_len - 1);
    strncat(out, rom_name, (size_t)out_len - 1 - strlen(out));
    strncat(out, ".st", (size_t)out_len - 1 - strlen(out));
}

static uint16_t rom_checksum(const uint8_t *rom, uint32_t rom_size) {
    if (rom_size < 0x150)
        return 0;
    return (uint16_t)(((uint16_t)rom[0x14E] << 8) | rom[0x14F]);
}

bool gbc_state_save(const struct PicoCalcAPI *api, const char *rom_name,
                    const void *gb_state, uint32_t gb_state_size,
                    const uint8_t *cart_ram, uint32_t cart_ram_size,
                    const uint8_t *rom, uint32_t rom_size) {
    if (!api || !rom_name || !rom_name[0])
        return false;

    gbc_state_header_t h;
    memcpy(h.magic, "GBST", 4);
    h.version       = STATE_VERSION;
    h.gb_size       = gb_state_size;
    h.cart_ram_size = cart_ram_size;
    h.rom_size      = rom_size;
    h.rom_checksum  = rom_checksum(rom, rom_size);
    h.reserved      = 0;

    char path[96];
    build_state_path(path, sizeof(path), rom_name);

    pcfile_t f = api->fs->open(path, "wb");
    if (!f)
        return false;
    bool ok =
        api->fs->write(f, &h, (int)sizeof(h)) == (int)sizeof(h) &&
        api->fs->write(f, gb_state, (int)gb_state_size) == (int)gb_state_size &&
        api->fs->write(f, cart_ram, (int)cart_ram_size) == (int)cart_ram_size;
    api->fs->close(f);
    return ok;
}

bool gbc_state_load(const struct PicoCalcAPI *api, const char *rom_name,
                    void *gb_state_out, uint32_t gb_state_size,
                    uint8_t *cart_ram_out, uint32_t cart_ram_size,
                    const uint8_t *rom, uint32_t rom_size) {
    if (!api || !rom_name || !rom_name[0])
        return false;

    char path[96];
    build_state_path(path, sizeof(path), rom_name);
    if (!api->fs->exists(path))
        return false;

    pcfile_t f = api->fs->open(path, "rb");
    if (!f)
        return false;

    gbc_state_header_t h;
    bool ok = api->fs->read(f, &h, (int)sizeof(h)) == (int)sizeof(h) &&
              memcmp(h.magic, "GBST", 4) == 0 &&
              h.version == STATE_VERSION &&
              h.gb_size == gb_state_size &&
              h.cart_ram_size == cart_ram_size &&
              h.rom_size == rom_size &&
              h.rom_checksum == rom_checksum(rom, rom_size);
    if (ok)
        ok = api->fs->read(f, gb_state_out, (int)gb_state_size) ==
                 (int)gb_state_size &&
             api->fs->read(f, cart_ram_out, (int)cart_ram_size) ==
                 (int)cart_ram_size;
    api->fs->close(f);
    return ok;
}
