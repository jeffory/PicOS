#include "fs.h"
#include "os.h"
#include <string.h>

static const PicoCalcAPI *s_api;

bool gbc_fs_init(GBCFilesystem *ctx) {
    s_api = NULL;
    memset(ctx, 0, sizeof(GBCFilesystem));
    return true;
}

void gbc_fs_set_api(const PicoCalcAPI *api) {
    s_api = api;
}

int gbc_fs_load_rom(GBCFilesystem *ctx, const char *path, uint8_t *rom_buf, int max_size) {
    if (!s_api) return 0;

    pcfile_t f = s_api->fs->open(path, "rb");
    if (!f) return 0;

    int bytes_read = s_api->fs->read(f, rom_buf, max_size);
    s_api->fs->close(f);

    if (bytes_read <= 0) return 0;
    
    const char *name = path;
    const char *last_slash = path;
    for (int i = 0; path[i]; i++) {
        if (path[i] == '/') last_slash = &path[i + 1];
    }
    name = last_slash;
    
    int name_len = 0;
    while (name[name_len] && name[name_len] != '.' && name_len < 15) {
        name_len++;
    }
    
    memset(ctx->current_rom_name, 0, 16);
    if (name_len > 0) {
        memcpy(ctx->current_rom_name, name, name_len);
    }
    
    return bytes_read;
}

bool gbc_fs_save_ram(GBCFilesystem *ctx, const uint8_t *ram, int size) {
    if (!s_api || size <= 0) return false;
    
    char save_path[64];
    int i = 0;
    const char *prefix = "/data/gbc/saves/";
    while (prefix[i]) {
        save_path[i] = prefix[i];
        i++;
    }
    
    int j = 0;
    while (ctx->current_rom_name[j] && j < 12) {
        save_path[i + j] = ctx->current_rom_name[j];
        j++;
    }
    save_path[i + j] = '.';
    save_path[i + j + 1] = 's';
    save_path[i + j + 2] = 'a';
    save_path[i + j + 3] = 'v';
    save_path[i + j + 4] = '\0';
    
    pcfile_t f = s_api->fs->open(save_path, "wb");
    if (!f) return false;
    
    s_api->fs->write(f, ram, size);
    s_api->fs->close(f);
    
    return true;
}

bool gbc_fs_load_ram(GBCFilesystem *ctx, uint8_t *ram, int size) {
    if (!s_api || size <= 0) return false;
    
    char save_path[64];
    int i = 0;
    const char *prefix = "/data/gbc/saves/";
    while (prefix[i]) {
        save_path[i] = prefix[i];
        i++;
    }
    
    int j = 0;
    while (ctx->current_rom_name[j] && j < 12) {
        save_path[i + j] = ctx->current_rom_name[j];
        j++;
    }
    save_path[i + j] = '.';
    save_path[i + j + 1] = 's';
    save_path[i + j + 2] = 'a';
    save_path[i + j + 3] = 'v';
    save_path[i + j + 4] = '\0';
    
    if (!s_api->fs->exists(save_path)) {
        return false;
    }
    
    pcfile_t f = s_api->fs->open(save_path, "rb");
    if (!f) return false;
    
    int bytes_read = s_api->fs->read(f, ram, size);
    s_api->fs->close(f);
    
    return bytes_read > 0;
}

void gbc_fs_get_rom_name(GBCFilesystem *ctx, char *name_out) {
    memcpy(name_out, ctx->current_rom_name, 16);
}
