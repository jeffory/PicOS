#pragma once

// ZIP entry-name validation (pure; no miniz, no SD).  Used by zip_util.c
// before extracting; host-tested in tests/unit/test_zip_name.c.

#include <stdbool.h>

#define ZIP_MAX_NAME 255  // entry name length

// Strict entry-name validation. Rejects NULL/empty, names longer than
// ZIP_MAX_NAME, leading '/', any '\\' or ':', control bytes, and any
// ".", ".." or empty path component. A trailing '/' (directory entry) is
// allowed. Note: unlike the old substring check, "foo..bar" is ACCEPTED —
// only exact "." / ".." components are traversal hazards.
bool zip_entry_name_valid(const char *name);
