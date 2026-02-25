#define UMM_BLOCK_BODY_SIZE 128
#define UMM_INLINE_METRICS

// ── Multi-core safety ─────────────────────────────────────────────────────────
// umm_malloc is called from both Core 0 (Lua) and Core 1 (Mongoose/WiFi).
// We use a Pico SDK critical_section_t (hardware spinlock + IRQ save/restore)
// to serialise all heap operations across cores.
//
// critical_section_t is non-recursive.  umm_malloc/free do NOT nest in
// practice: umm_realloc handles the NULL-ptr and zero-size edge cases BEFORE
// acquiring the lock, so there is no nested acquisition path.
#include "pico/critical_section.h"

// Defined in lua_psram_alloc.c and initialised in lua_psram_alloc_init().
extern critical_section_t g_umm_critsec;

#define UMM_CRITICAL_DECL(tag)
#define UMM_CRITICAL_ENTRY(tag)  critical_section_enter_blocking(&g_umm_critsec)
#define UMM_CRITICAL_EXIT(tag)   critical_section_exit(&g_umm_critsec)
