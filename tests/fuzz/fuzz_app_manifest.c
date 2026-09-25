// libFuzzer target for src/os/app_manifest.c: the launcher's app.json reader
// over arbitrary bytes (not NUL-terminated: the input buffer is exactly
// `size` bytes, so ASan catches any read past it).  Also checks that a
// parsed id either passes app_manifest_id_valid() or is rejected by it
// without crashing, and that every output field is NUL-terminated.
#include "app_manifest.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  app_entry_t app;
  memset(&app, 0x5a, sizeof(app));
  app_manifest_parse((const char *)data, size, "fuzzdir", &app);
  if (memchr(app.id, 0, sizeof(app.id)) == NULL ||
      memchr(app.name, 0, sizeof(app.name)) == NULL ||
      memchr(app.description, 0, sizeof(app.description)) == NULL ||
      memchr(app.version, 0, sizeof(app.version)) == NULL ||
      memchr(app.category, 0, sizeof(app.category)) == NULL)
    abort();
  (void)app_manifest_id_valid(app.id);
  return 0;
}
