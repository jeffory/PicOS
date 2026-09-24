#pragma once

// app.json reader for the launcher.
//
// A minimal, bounded reader for the flat manifest format (no cJSON: flash
// savings).  Pure: no SD access, no allocation.  Used by launcher.c;
// host-tested in tests/unit/test_app_manifest.c and fuzzed by
// tests/fuzz/fuzz_app_manifest.c.

#include <stdbool.h>
#include <stddef.h>

#include "launcher_types.h"

// Fill the manifest fields of *app (id, name, description, version,
// category, requirements list + flags, system_clock_khz, min_psram_kb) from
// json[0..len).  Missing keys get the launcher's defaults: id
// "local.<dir_name>", name <dir_name>, version "1.0", empty description
// and category, no requirements.  Stops at len or a NUL, whichever is first.
// String values decode JSON escapes (\" \\ \/ \b \f \n \r \t \uXXXX;
// non-ASCII \u escapes become '?').  Does not touch path, type or icon.
// requirements holds the folded names from the "requirements" array,
// space-separated.
void app_manifest_parse(const char *json, size_t len, const char *dir_name,
                        app_entry_t *app);

// The same fields when there is no readable app.json: id "local.<dir>",
// name <dir>, version "?".
void app_manifest_defaults(const char *dir_name, app_entry_t *app);

// An app id safe to use as a path component (/data/<id>/...):
// non-empty, shorter than app_entry_t.id, only [A-Za-z0-9._-], and neither
// "." nor containing "..".
bool app_manifest_id_valid(const char *id);

// Is `name` one of the space-separated names in `list` (app_entry_t
// .requirements)?  Whole names only.
bool app_requirements_has(const char *list, const char *name);
