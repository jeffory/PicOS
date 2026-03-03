#include "native_loader.h"
#include "launcher_types.h"
#include "app_abi.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"
#include "../drivers/sdcard.h"
#include "../os/os.h"

#include "umm_malloc.h"
#include "pico/stdlib.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

// RP2350 XIP address ranges for PSRAM CS1.
// umm_malloc returns addresses in the cached alias (0x11xxxxxx).
// All code writes and execution must use the uncached alias (0x15xxxxxx)
// to avoid XIP write-back cache coherency issues.
#define PSRAM_CS1_CACHED_BASE   0x11000000u
#define PSRAM_CS1_CACHED_END    0x12000000u
#define PSRAM_CACHED_TO_UNCACHED 0x04000000u  // add to get uncached alias

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
static uint8_t s_native_stack[NATIVE_STACK_SIZE] __attribute__((aligned(8)));

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

static bool native_run(const app_entry_t *app) {
  printf("[NATIVE] Loading '%s'\n", app->name);

  // ── 1. Read ELF from SD card ──────────────────────────────────────────────
  char elf_path[160];
  snprintf(elf_path, sizeof(elf_path), "%s/main.elf", app->path);

  int file_len = 0;
  uint8_t *file_buf = (uint8_t *)sdcard_read_file(elf_path, &file_len);
  if (!file_buf) {
    show_error("Failed to load native app:", elf_path);
    return false;
  }
  printf("[NATIVE] ELF: %d bytes\n", file_len);

  // ── 2. Validate ELF header ────────────────────────────────────────────────
  if (file_len < (int)sizeof(Elf32_Ehdr)) {
    umm_free(file_buf);
    show_error("ELF: file too small", NULL);
    return false;
  }

  const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)file_buf;

  if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
      ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3) {
    umm_free(file_buf);
    show_error("ELF: bad magic", NULL);
    return false;
  }
  if (ehdr->e_type != ET_DYN) {
    umm_free(file_buf);
    show_error("ELF: must be PIE (ET_DYN)", NULL);
    return false;
  }
  if (ehdr->e_machine != EM_ARM) {
    umm_free(file_buf);
    show_error("ELF: must be ARM", NULL);
    return false;
  }

  // ── 3. Measure PT_LOAD virtual address range ──────────────────────────────
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
    if (seg_end > mem_max)
      mem_max = seg_end;
    found_load = true;
  }

  if (!found_load) {
    umm_free(file_buf);
    show_error("ELF: no PT_LOAD segments", NULL);
    return false;
  }

  uint32_t image_size = mem_max - mem_min;
  printf("[NATIVE] Image: %lu bytes (vaddr 0x%08lx..0x%08lx)\n",
         (unsigned long)image_size,
         (unsigned long)mem_min, (unsigned long)mem_max);

  // ── 4. Allocate load image in PSRAM ──────────────────────────────────────
  uint8_t *load_base = (uint8_t *)umm_malloc(image_size);
  if (!load_base) {
    umm_free(file_buf);
    show_error("ELF: out of PSRAM", NULL);
    return false;
  }

  // ── 4b. Switch to uncached PSRAM alias for all writes and execution ────────
  // umm_malloc returns a cached alias (0x11xxxxxx).  Writing code through the
  // XIP write-back cache and then fetching instructions from the same alias
  // causes hard faults: dirty cache lines may not be committed to physical
  // PSRAM, so instruction fetch reads stale data.
  //
  // Solution: use the uncached CS1 alias (0x15xxxxxx) for all writes and for
  // computing the entry point.  Reads/writes bypass the XIP cache entirely and
  // go directly to PSRAM.  We keep load_base for umm_free() at the end.
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
    uint8_t *dest       = exec_base + (ph->p_vaddr - mem_min);
    const uint8_t *src  = file_buf + ph->p_offset;
    memcpy(dest, src, ph->p_filesz);
  }

  // ── 6. Apply relocations ──────────────────────────────────────────────────
  // Load bias: add this to any virtual address to get the runtime address.
  // Use exec_base so relocated pointers point into the uncached alias too.
  uint32_t load_bias = (uint32_t)exec_base - mem_min;

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
          uint32_t *target =
              (uint32_t *)(exec_base + (rel[j].r_offset - mem_min));
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
          uint32_t *target =
              (uint32_t *)(exec_base + (rela[j].r_offset - mem_min));
          *target = load_bias + (uint32_t)rela[j].r_addend;
        }
      }
    }

    break; // Only one PT_DYNAMIC segment expected
  }

  // ── 7. Compute Thumb-2 entry point ────────────────────────────────────────
  // e_entry may have the Thumb bit set (bit 0 = 1); strip it for the offset
  // calculation, then re-apply for the function pointer call convention.
  uintptr_t entry_voff = (ehdr->e_entry & ~1u) - mem_min;
  uintptr_t entry_addr = (uintptr_t)exec_base + entry_voff;
  entry_addr |= 1u; // Thumb mode

  printf("[NATIVE] Entry %p (thumb)\n", (void *)entry_addr);

  // Sanity-check: print first 16 bytes of exec_base so we can verify the
  // image is intact just before launch (useful for catching PSRAM corruption).
  printf("[NATIVE] exec_base[0..15]:");
  for (int i = 0; i < 16; i++)
    printf(" %02x", exec_base[i]);
  printf("\n");

  // NOTE: file_buf is intentionally freed AFTER entry_fn returns, not before.
  // Freeing it here (while Core 1 runs Mongoose via umm_malloc/umm_free every
  // 5 ms) creates a race on the umm heap metadata.  A corrupted heap can cause
  // Core 1 to overwrite exec_base PSRAM with network data, trashing the app's
  // literal pool and format-string pointers before/during the first loop iter.
  // Deferring the free costs ~file_len bytes of PSRAM for the app's lifetime
  // but eliminates the race window entirely.

  // Clear any stale keyboard state from the launcher before starting the app.
  // This matches what lua_bridge.c does before running Lua apps.
  kbd_clear_state();

  // ── 8. Launch app ─────────────────────────────────────────────────────────
  // Enable blocking flush BEFORE the initial clear so no DMA is in flight
  // when entry_fn jumps to PSRAM. The clear flush must complete fully or the
  // CPU stalls on instruction fetches while DMA holds the PSRAM SPI bus.
  g_display_flush_blocking = true;
  display_clear(C_BG);
  display_flush();

  picos_app_entry_t entry_fn = (picos_app_entry_t)entry_addr;

  // Run the app on PSP so interrupt handlers keep using MSP.
  // See launch_on_psp() above for the full rationale.
  uint32_t stack_top = (uint32_t)(s_native_stack + NATIVE_STACK_SIZE);
  launch_on_psp(stack_top, entry_fn,
                (const PicoCalcAPI *)&g_api, app->path, app->id, app->name);

  g_display_flush_blocking = false;
  printf("[NATIVE] App '%s' returned\n", app->name);

  // ── 9. Free both buffers now that the app has exited ──────────────────────
  umm_free(file_buf);    // safe to free now — app has finished executing
  umm_free(load_base);   // use original cached-alias address for the allocator
  return true;
}

static bool native_can_handle(const app_entry_t *app) {
  return app->type == APP_TYPE_NATIVE;
}

const AppRunner g_native_runner = {"native", native_can_handle, native_run};
