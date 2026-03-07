#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "os.h"

typedef struct {
    void *file_handle;
    char current_rom_name[16];
} GBCFilesystem;

bool gbc_fs_init(GBCFilesystem *ctx);
void gbc_fs_set_api(const struct PicoCalcAPI *api);
int  gbc_fs_load_rom(GBCFilesystem *ctx, const char *path, uint8_t *rom_buf, int max_size);
bool gbc_fs_save_ram(GBCFilesystem *ctx, const uint8_t *ram, int size);
bool gbc_fs_load_ram(GBCFilesystem *ctx, uint8_t *ram, int size);
void gbc_fs_get_rom_name(GBCFilesystem *ctx, char *name_out);
