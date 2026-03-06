#include "native_loader.h"
#include "launcher_types.h"
#include "app_abi.h"
#include "../drivers/audio.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"
#include "../drivers/sdcard.h"
#include "../os/os.h"

#include "umm_malloc.h"
#include "pico/stdlib.h"
#include "hardware/xip_cache.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

// RP2350 XIP address ranges for PSRAM CS1.
// umm_malloc returns addresses in the cached alias (0x11xxxxxx).
// Code is WRITTEN through the uncached alias (0x15xxxxxx) to guarantee
// writes reach physical PSRAM (bypassing the write-back cache).  After
// writing, the XIP cache is invalidated and execution uses the CACHED
// alias (0x11xxxxxx) so the 16KB XIP cache serves most instruction
// fetches — dramatically reducing QMI bus traffic and eliminating the
// random IBUSERR/PRECISERR faults seen with uncached execution.
#define PSRAM_CS1_CACHED_BASE    0x11000000u
#define PSRAM_CS1_CACHED_END     0x12000000u
#define PSRAM_CACHED_TO_UNCACHED 0x04000000u  // add to get uncached alias

// Maximum virtual address range accepted for a native app image.
// Rejects malformed or malicious ELFs before attempting a heap allocation.
// 2 MB is generous — real apps are well under 512 KB.
#define NATIVE_MAX_IMAGE_SIZE (2u * 1024u * 1024u)

// =============================================================================
// Minimal ELF32 type definitions
// (newlib-arm headers don't always ship <elf.h>)
// =============================================================================

typedef uint32_t Elf32_Addr;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;
typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;

#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
} Elf32_Phdr;

typedef struct {
    Elf32_Sword d_tag;
    union {
        Elf32_Word d_val;
        Elf32_Addr d_ptr;
    } d_un;
} Elf32_Dyn;

typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
} Elf32_Rel;

typedef struct {
    Elf32_Addr  r_offset;
    Elf32_Word  r_info;
    Elf32_Sword r_addend;
} Elf32_Rela;

// ELF constants
#define ELFMAG0  0x7fu
#define ELFMAG1  'E'
#define ELFMAG2  'L'
#define ELFMAG3  'F'
#define ET_DYN   3
#define EM_ARM   40
#define PT_LOAD  1
#define PT_DYNAMIC 2
#define DT_NULL  0
#define DT_REL   17
#define DT_RELSZ 18
#define DT_RELA  7
#define DT_RELASZ 8
// R_ARM_RELATIVE (type 23): *target += load_base_offset
#define R_ARM_RELATIVE  23
#define ELF32_R_TYPE(i) ((i) & 0xffu)

// =============================================================================
// Helpers
// =============================================================================

#define C_BG COLOR_BLACK

extern PicoCalcAPI g_api;

static void show_error(const char *line1, const char *line2) {
  display_clear(C_BG);
  display_draw_text(8, 8, line1, COLOR_RED, C_BG);
  if (line2)
    display_draw_text(8, 20, line2, COLOR_WHITE, C_BG);
  display_flush();
  sleep_ms(3000);
}

// =============================================================================
// App stack (PSP-based isolation)
// =============================================================================

// Native apps run on the PSP (Process Stack Pointer).  Interrupt handlers
// always use the MSP (Main Stack Pointer) regardless of SPSEL, so the two
// stacks are completely independent: app stack pressure and interrupt stacking
// do not interfere with each other.
//
// 8 KB gives comfortable headroom for the GBC emulator's call depth and any
// local arrays allocated on the stack.  The buffer lives in SRAM (fast) and
// is a module-level static so it is zero-initialised.
#define NATIVE_STACK_SIZE (8 * 1024)
uint8_t s_native_stack[NATIVE_STACK_SIZE] __attribute__((aligned(8)));

// Stack canary: the bottom NATIVE_STACK_GUARD_WORDS words are filled with a
// sentinel before launch and checked afterwards.  If the stack overflows into
// this guard zone the corruption is detected and reported.  The stack grows
// downward from s_native_stack+NATIVE_STACK_SIZE, so the bottom of the buffer
// is the last area to be reached by overflow.
#define NATIVE_STACK_CANARY      0xDEADBEEFu
#define NATIVE_STACK_GUARD_WORDS 8   // 32 bytes

// launch_on_psp() — naked trampoline that:
//   1. Saves r4-r7 + LR onto the current MSP (OS stack).
//   2. Loads the two extra args (app_id, app_name) before switching stacks.
//   3. Sets PSP = psp_top and sets CONTROL.SPSEL=1 so Thread mode uses PSP.
//   4. Calls fn(api, app_dir, app_id, app_name) — runs entirely on PSP.
//   5. Clears CONTROL.SPSEL=0 to restore Thread mode to MSP.
//   6. Pops r4-r7 + PC from MSP and returns to native_run normally.
//
// Signature (AAPCS):
//   r0  = psp_top   (top of app stack buffer)
//   r1  = fn        (Thumb entry point, bit-0 = 1)
//   r2  = api       (1st app arg)
//   r3  = app_dir   (2nd app arg)
//   [sp+0]  = app_id   (3rd app arg, on caller's stack before this push)
//   [sp+4]  = app_name (4th app arg)
__attribute__((naked, noinline))
static void launch_on_psp(uint32_t psp_top, picos_app_entry_t fn,
                          const PicoCalcAPI *api, const char *app_dir,
                          const char *app_id, const char *app_name)
{
    __asm__ (
        "push   {r4-r7, lr}     \n\t"   /* save callee-saved + LR on MSP     */
        "ldr    r4, [sp, #20]   \n\t"   /* load app_id   (was [sp+0] pre-push)*/
        "ldr    r5, [sp, #24]   \n\t"   /* load app_name (was [sp+4] pre-push)*/
        "mov    r6, r1          \n\t"   /* save fn before r1 is clobbered     */
        "mrs    r7, control     \n\t"   /* save original CONTROL register     */
        /* ── switch Thread mode to PSP ──────────────────────────────── */
        "msr    psp, r0         \n\t"   /* PSP = psp_top                      */
        "orr    r0, r7, #2      \n\t"   /* CONTROL | SPSEL                    */
        "msr    control, r0     \n\t"   /* SPSEL = 1 → Thread uses PSP        */
        "isb                    \n\t"   /* sync pipeline after CONTROL write  */
        /* ── call fn(api, app_dir, app_id, app_name) ────────────────── */
        "mov    r0, r2          \n\t"
        "mov    r1, r3          \n\t"
        "mov    r2, r4          \n\t"
        "mov    r3, r5          \n\t"
        "blx    r6              \n\t"   /* app runs here on PSP               */
        /* r4-r7 are callee-saved so entry_fn has restored them         */
        /* ── restore Thread mode to MSP ─────────────────────────────── */
        "mrs    r0, control     \n\t"
        "bic    r0, r0, #2      \n\t"   /* clear SPSEL                        */
        "msr    control, r0     \n\t"   /* SPSEL = 0 → Thread uses MSP again  */
        "isb                    \n\t"
        "pop    {r4-r7, pc}     \n\t"   /* restore from MSP, return           */
    );
}

// =============================================================================
// ELF loader
// =============================================================================

// Declared in main.c — pauses Core 1's Mongoose/WiFi polling loop.
extern volatile bool g_core1_pause;

static bool native_run(const app_entry_t *app) {
  printf("[NATIVE] Loading '%s'\n", app->name);

  // Pause Core 1 to eliminate PSRAM heap contention during ELF loading.
  // Core 1 runs umm_malloc/umm_free every 5ms for Mongoose; those allocations
  // share the same PSRAM heap where the app image is loaded.  Without this
  // pause, heap metadata corruption can cause Core 1 to overwrite app code.
  g_core1_pause = true;
  sleep_ms(10); // let any in-flight Core 1 umm operation complete

  // ── 1. Read ELF from SD card ──────────────────────────────────────────────
  char elf_path[160];
  snprintf(elf_path, sizeof(elf_path), "%s/main.elf", app->path);

  bool ok = false;
  uint8_t *load_base = NULL;

  int file_len = 0;
  uint8_t *file_buf = (uint8_t *)sdcard_read_file(elf_path, &file_len);
  if (!file_buf) {
    show_error("Failed to load native app:", elf_path);
    goto out;
  }
  printf("[NATIVE] ELF: %d bytes\n", file_len);

  // ── 2. Validate ELF header ────────────────────────────────────────────────
  if (file_len < (int)sizeof(Elf32_Ehdr)) {
    show_error("ELF: file too small", NULL);
    goto out;
  }

  const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)file_buf;

  if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
      ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3) {
    show_error("ELF: bad magic", NULL);
    goto out;
  }
  if (ehdr->e_type != ET_DYN) {
    show_error("ELF: must be PIE (ET_DYN)", NULL);
    goto out;
  }
  if (ehdr->e_machine != EM_ARM) {
    show_error("ELF: must be ARM", NULL);
    goto out;
  }

  // ── 3. Measure PT_LOAD virtual address range ──────────────────────────────
  uint32_t phdr_end = (uint32_t)ehdr->e_phoff +
                      (uint32_t)ehdr->e_phentsize * (uint32_t)ehdr->e_phnum;
  if (phdr_end > (uint32_t)file_len) {
    show_error("ELF: phdr table out of bounds", NULL);
    goto out;
  }

  const Elf32_Phdr *phdr_table =
      (const Elf32_Phdr *)(file_buf + ehdr->e_phoff);

  Elf32_Addr mem_min = 0xFFFFFFFFu;
  Elf32_Addr mem_max = 0;
  bool found_load = false;

  for (int i = 0; i < ehdr->e_phnum; i++) {
    const Elf32_Phdr *ph = &phdr_table[i];
    if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
      continue;
    if (ph->p_vaddr < mem_min)
      mem_min = ph->p_vaddr;
    Elf32_Addr seg_end = ph->p_vaddr + ph->p_memsz;
    if (seg_end < ph->p_vaddr) {
      show_error("ELF: segment vaddr overflow", NULL);
      goto out;
    }
    if (seg_end > mem_max)
      mem_max = seg_end;
    found_load = true;
  }

  if (!found_load) {
    show_error("ELF: no PT_LOAD segments", NULL);
    goto out;
  }

  uint32_t image_size = mem_max - mem_min;
  printf("[NATIVE] Image: %lu bytes (vaddr 0x%08lx..0x%08lx)\n",
         (unsigned long)image_size,
         (unsigned long)mem_min, (unsigned long)mem_max);

  if (image_size > NATIVE_MAX_IMAGE_SIZE) {
    show_error("ELF: image too large (>2MB)", NULL);
    goto out;
  }

  // ── 4. Allocate load image in PSRAM ──────────────────────────────────────
  load_base = (uint8_t *)umm_malloc(image_size);
  if (!load_base) {
    show_error("ELF: out of PSRAM", NULL);
    goto out;
  }

  // ── 4a'. Flush dirty cache lines from umm_malloc ─────────────────────────
  // umm_malloc modifies PSRAM heap metadata through the cached alias,
  // creating dirty cache lines.  We must flush them to PSRAM *before*
  // writing app data through the uncached alias (step 5), otherwise:
  //   - dirty metadata lines can conflict with uncached writes when they
  //     share the same 8-byte cache line (write-back eviction overwrites
  //     the uncached data → app code corruption)
  //   - the final xip_cache_invalidate_all (step 7) would discard unflushed
  //     metadata → stale heap state → subsequent umm_malloc calls (e.g.
  //     inside sdcard_list_dir) return overlapping pointers → crash
  __asm volatile ("dsb sy");
  xip_cache_clean_all();
  __asm volatile ("isb sy");

  // ── 4b. Compute uncached alias for writes ──────────────────────────────────
  // umm_malloc returns a cached alias (0x11xxxxxx).  Writing code through the
  // XIP write-back cache risks stale instruction fetches, so all writes go
  // through the uncached alias (0x15xxxxxx) to guarantee data reaches PSRAM.
  // After writing, the XIP cache is invalidated (step 7) and execution uses
  // the cached alias — the 16KB XIP cache serves most fetches, avoiding the
  // IBUSERR/PRECISERR faults caused by hammering QMI with every fetch.
  uint8_t *exec_base = load_base;
  if ((uintptr_t)load_base >= PSRAM_CS1_CACHED_BASE &&
      (uintptr_t)load_base <  PSRAM_CS1_CACHED_END) {
    exec_base = load_base + PSRAM_CACHED_TO_UNCACHED;
  }
  printf("[NATIVE] load_base %p  exec_base %p\n",
         (void *)load_base, (void *)exec_base);

  // ── 5. Zero image (covers BSS), copy PT_LOAD segments ─────────────────────
  memset(exec_base, 0, image_size);

  for (int i = 0; i < ehdr->e_phnum; i++) {
    const Elf32_Phdr *ph = &phdr_table[i];
    if (ph->p_type != PT_LOAD || ph->p_filesz == 0)
      continue;
    if (ph->p_offset + ph->p_filesz > (uint32_t)file_len) {
      show_error("ELF: segment data out of bounds", NULL);
      goto out;
    }
    if (ph->p_vaddr - mem_min + ph->p_memsz > image_size) {
      show_error("ELF: segment exceeds image", NULL);
      goto out;
    }
    uint8_t *dest       = exec_base + (ph->p_vaddr - mem_min);
    const uint8_t *src  = file_buf + ph->p_offset;
    memcpy(dest, src, ph->p_filesz);
  }

  // ── 6. Apply relocations ──────────────────────────────────────────────────
  // Load bias: add this to any virtual address to get the runtime address.
  // Use load_base (cached alias) so relocated pointers point into the cached
  // address space where the app will execute.  The writes themselves go
  // through exec_base (uncached) to ensure they reach physical PSRAM.
  uint32_t load_bias = (uint32_t)load_base - mem_min;

  for (int i = 0; i < ehdr->e_phnum; i++) {
    const Elf32_Phdr *ph = &phdr_table[i];
    if (ph->p_type != PT_DYNAMIC)
      continue;

    const Elf32_Dyn *dyn =
        (const Elf32_Dyn *)(exec_base + (ph->p_vaddr - mem_min));

    Elf32_Addr rel_addr = 0;  Elf32_Word rel_size = 0;
    Elf32_Addr rela_addr = 0; Elf32_Word rela_size = 0;

    for (const Elf32_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
      switch (d->d_tag) {
        case DT_REL:    rel_addr  = d->d_un.d_ptr; break;
        case DT_RELSZ:  rel_size  = d->d_un.d_val; break;
        case DT_RELA:   rela_addr = d->d_un.d_ptr; break;
        case DT_RELASZ: rela_size = d->d_un.d_val; break;
        default: break;
      }
    }

    // Elf32_Rel — implicit addend: *target += load_bias
    if (rel_addr && rel_size) {
      const Elf32_Rel *rel =
          (const Elf32_Rel *)(exec_base + (rel_addr - mem_min));
      uint32_t count = rel_size / sizeof(Elf32_Rel);
      for (uint32_t j = 0; j < count; j++) {
        if (ELF32_R_TYPE(rel[j].r_info) == R_ARM_RELATIVE) {
          uint32_t roff = rel[j].r_offset - mem_min;
          if (roff + sizeof(uint32_t) > image_size) continue;
          uint32_t *target = (uint32_t *)(exec_base + roff);
          *target += load_bias;
        }
      }
    }

    // Elf32_Rela — explicit addend: *target = load_bias + r_addend
    if (rela_addr && rela_size) {
      const Elf32_Rela *rela =
          (const Elf32_Rela *)(exec_base + (rela_addr - mem_min));
      uint32_t count = rela_size / sizeof(Elf32_Rela);
      for (uint32_t j = 0; j < count; j++) {
        if (ELF32_R_TYPE(rela[j].r_info) == R_ARM_RELATIVE) {
          uint32_t roff = rela[j].r_offset - mem_min;
          if (roff + sizeof(uint32_t) > image_size) continue;
          uint32_t *target = (uint32_t *)(exec_base + roff);
          *target = load_bias + (uint32_t)rela[j].r_addend;
        }
      }
    }

    break; // Only one PT_DYNAMIC segment expected
  }

  // ── 7. Flush XIP cache and compute entry point ──────────────────────────
  // All code/data was written through the uncached alias (exec_base) so it
  // is committed to physical PSRAM.  The cache was cleaned in step 4a' so
  // there are no dirty lines to lose.  Invalidate the XIP cache so that
  // instruction fetches and data reads from the cached alias (load_base)
  // will pull fresh data from PSRAM and cache it.
  __asm volatile ("dsb sy");  // ensure all uncached writes complete
  xip_cache_invalidate_all();
  __asm volatile ("isb sy");  // sync pipeline after cache invalidation

  // e_entry may have the Thumb bit set (bit 0 = 1); strip it for the offset
  // calculation, then re-apply for the function pointer call convention.
  // Entry point uses load_base (cached alias) — NOT exec_base (uncached).
  uintptr_t entry_voff = (ehdr->e_entry & ~1u) - mem_min;
  if (entry_voff >= image_size) {
    show_error("ELF: entry point out of bounds", NULL);
    goto out;
  }
  uintptr_t entry_addr = (uintptr_t)load_base + entry_voff;
  entry_addr |= 1u; // Thumb mode

  printf("[NATIVE] Entry %p (thumb, cached)\n", (void *)entry_addr);

  // Sanity-check: print first 16 bytes via cached alias to verify the
  // XIP cache serves correct data after invalidation.
  printf("[NATIVE] load_base[0..15]:");
  for (int i = 0; i < 16; i++)
    printf(" %02x", load_base[i]);
  printf("\n");

  // Clear any stale keyboard state from the launcher before starting the app.
  // This matches what lua_bridge.c does before running Lua apps.
  kbd_clear_state();

  // ── 8. Launch app ─────────────────────────────────────────────────────────
  // Non-blocking flush is safe: DMA reads from SRAM framebuffers (AHB) while
  // the CPU fetches app code from PSRAM (QMI/XIP) — separate buses, no
  // contention.  Double buffering prevents CPU/DMA framebuffer conflicts.

  display_clear(C_BG);
  display_flush();

  picos_app_entry_t entry_fn = (picos_app_entry_t)entry_addr;

  // Run the app on PSP so interrupt handlers keep using MSP.
  // See launch_on_psp() above for the full rationale.

  // Plant canary words at the bottom (low-address end) of the stack buffer.
  // If the app overflows the stack downward far enough to reach this zone the
  // corruption will be visible after the app returns.
  uint32_t *guard = (uint32_t *)s_native_stack;
  for (int i = 0; i < NATIVE_STACK_GUARD_WORDS; i++)
    guard[i] = NATIVE_STACK_CANARY;

  uint32_t stack_top = (uint32_t)(s_native_stack + NATIVE_STACK_SIZE);
  launch_on_psp(stack_top, entry_fn,
                (const PicoCalcAPI *)&g_api, app->path, app->id, app->name);

  // Check canary after app returns.  A corrupted word means the stack grew
  // past the guard zone — report it so developers know to investigate.
  for (int i = 0; i < NATIVE_STACK_GUARD_WORDS; i++) {
    if (guard[i] != NATIVE_STACK_CANARY) {
      printf("[NATIVE] WARNING: stack overflow detected in '%s' "
             "(canary[%d] = 0x%08lx)\n",
             app->name, i, (unsigned long)guard[i]);
      break;
    }
  }

  printf("[NATIVE] App '%s' returned\n", app->name);
  ok = true;

out:
  // ── 9. Cleanup and resume Core 1 ──────────────────────────────────────────
  audio_stop_stream();   // ensure audio streaming is stopped on exit/crash
  audio_stop_tone();
  if (load_base)
    umm_free(load_base);
  if (file_buf)
    umm_free(file_buf);
  g_core1_pause = false;

  return ok;
}

static bool native_can_handle(const app_entry_t *app) {
  return app->type == APP_TYPE_NATIVE;
}

const AppRunner g_native_runner = {"native", native_can_handle, native_run};
