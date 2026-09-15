#include "font_registry.h"
#include "font_6x8.h"
#include "font_8x12.h"
#include "font_scientifica.h"
#include "sdcard.h"
#include "umm_malloc.h"
#include <stdio.h>

#define LOADED_SLOTS (FONT_REGISTRY_SLOTS - FONT_REGISTRY_BUILTIN)

static const pc_font_t s_builtin[FONT_REGISTRY_BUILTIN] = {
  { 0x20, 0x7E, FONT_6X8_HEIGHT,  FONT_6X8_WIDTH,  1, NULL, &font_6x8[0][0],             NULL },
  { 0x20, 0x7E, FONT_8X12_HEIGHT, FONT_8X12_WIDTH, 1, NULL, &font_8x12[0][0],            NULL },
  { FONT_SCI_FIRST, FONT_SCI_LAST, FONT_SCI_HEIGHT, FONT_SCI_WIDTH, 1, NULL, &font_scientifica[0][0],      NULL },
  { FONT_SCI_FIRST, FONT_SCI_LAST, FONT_SCI_HEIGHT, FONT_SCI_WIDTH, 1, NULL, &font_scientifica_bold[0][0], NULL },
};

static pc_font_t s_loaded[LOADED_SLOTS];
static bool      s_live[LOADED_SLOTS];

const pc_font_t *font_registry_get(int id) {
  if (id >= 0 && id < FONT_REGISTRY_BUILTIN) return &s_builtin[id];
  int i = id - FONT_REGISTRY_BUILTIN;
  if (i < 0 || i >= LOADED_SLOTS || !s_live[i]) return NULL;
  return &s_loaded[i];
}

int font_registry_load(const char *path) {
  if (!path) return -1;
  int slot = -1;
  for (int i = 0; i < LOADED_SLOTS; i++) {
    if (!s_live[i]) { slot = i; break; }
  }
  if (slot < 0) {
    printf("[FONT] load %s: no free slot (%d loaded)\n", path, LOADED_SLOTS);
    return -1;
  }
  int len = 0;
  char *blob = sdcard_read_file(path, &len);
  if (!blob) {
    printf("[FONT] load %s: cannot read file\n", path);
    return -1;
  }
  pc_font_t f;
  if (!font_from_blob(&f, blob, (size_t)len)) {
    printf("[FONT] load %s: not a valid .pfn (%d bytes)\n", path, len);
    umm_free(blob);
    return -1;
  }
  f.blob = blob;
  s_loaded[slot] = f;
  s_live[slot] = true;
  printf("[FONT] loaded %s -> id %d (%dx%d, %d glyphs%s)\n", path,
         FONT_REGISTRY_BUILTIN + slot, f.max_width, f.height,
         font_glyph_count(&f), f.widths ? ", proportional" : "");
  return FONT_REGISTRY_BUILTIN + slot;
}

void font_registry_unload(int id) {
  int i = id - FONT_REGISTRY_BUILTIN;
  if (i < 0 || i >= LOADED_SLOTS || !s_live[i]) return;
  umm_free(s_loaded[i].blob);
  s_loaded[i].blob = NULL;
  s_live[i] = false;
}

void font_registry_unload_all(void) {
  for (int i = 0; i < LOADED_SLOTS; i++)
    font_registry_unload(FONT_REGISTRY_BUILTIN + i);
}
