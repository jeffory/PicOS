// PicOS-specific miniz configuration.
// Redirect heap allocations to umm_malloc (8MB PSRAM) instead of
// standard malloc (~28KB SRAM heap).
#pragma once

#include "umm_malloc.h"

#define MZ_MALLOC(x)     umm_malloc(x)
#define MZ_FREE(x)       umm_free(x)
#define MZ_REALLOC(p, x) umm_realloc(p, x)
