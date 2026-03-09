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
#include <stdlib.h>
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
// 5 MB is generous — real apps are well under 512 KB, but DOOM needs ~4MB.
#define NATIVE_MAX_IMAGE_SIZE (5u * 1024u * 1024u)

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
#define PF_X     0x1u   // Executable segment flag
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
  uint8_t *code_buf = NULL;

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

  // Also identify code vs data segments for split loading
  int code_seg_idx = -1;  // PT_LOAD with PF_X
  Elf32_Addr code_vaddr = 0, code_vend = 0;
  uint32_t code_memsz = 0;

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

    if ((ph->p_flags & PF_X) && code_seg_idx < 0) {
      code_seg_idx = i;
      code_vaddr = ph->p_vaddr;
      code_vend  = seg_end;
      code_memsz = ph->p_memsz;
    }
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
    show_error("ELF: image too large (>5MB)", NULL);
    goto out;
  }

  // ── 4. Split allocation: code in SRAM, data/BSS in PSRAM ────────────────
  //
  // If the code segment fits in SRAM (malloc), place it there for fast
  // instruction fetch without XIP cache pressure.  Data/BSS stays in PSRAM.
  // If SRAM malloc fails, fall back to the original all-PSRAM path.
  bool split_mode = false;

  // Only attempt SRAM split for small code segments.  The SRAM heap is
  // ~28KB and the SDK malloc panics (instead of returning NULL) on failure,
  // so we must not speculatively call malloc() for large allocations.
  #define MAX_SRAM_CODE_SIZE  (16u * 1024)
  if (code_seg_idx >= 0 && code_memsz > 0 && code_memsz <= MAX_SRAM_CODE_SIZE) {
    code_buf = malloc(code_memsz);
    if (code_buf) {
      split_mode = true;
      printf("[NATIVE] Split mode: code %lu bytes in SRAM @ %p\n",
             (unsigned long)code_memsz, (void *)code_buf);
    } else {
      printf("[NATIVE] SRAM alloc failed for code (%lu bytes), using all-PSRAM\n",
             (unsigned long)code_memsz);
    }
  }

  // PSRAM allocation: in split mode, only data/BSS; otherwise entire image
  uint32_t psram_size = split_mode ? (image_size - code_memsz) : image_size;
  // data_vaddr_start: first vaddr byte NOT in the code segment
  Elf32_Addr data_vaddr_start = split_mode ? code_vend : mem_min;

  if (psram_size > 0) {
    load_base = (uint8_t *)umm_malloc(psram_size);
    if (!load_base) {
      if (split_mode) { free(code_buf); code_buf = NULL; }
      show_error("ELF: out of PSRAM", NULL);
      goto out;
    }
  }

  // ── 4a'. Flush dirty cache lines from umm_malloc ─────────────────────────
  __asm volatile ("dsb sy");
  xip_cache_clean_all();
  __asm volatile ("isb sy");

  // ── 4b. Compute uncached alias for PSRAM writes ───────────────────────────
  uint8_t *exec_base = load_base;
  if (load_base &&
      (uintptr_t)load_base >= PSRAM_CS1_CACHED_BASE &&
      (uintptr_t)load_base <  PSRAM_CS1_CACHED_END) {
    exec_base = load_base + PSRAM_CACHED_TO_UNCACHED;
  }

  if (split_mode) {
    printf("[NATIVE] code_buf %p (SRAM)  load_base %p  exec_base %p (PSRAM data)\n",
           (void *)code_buf, (void *)load_base, (void *)exec_base);
  } else {
    printf("[NATIVE] load_base %p  exec_base %p\n",
           (void *)load_base, (void *)exec_base);
  }

  // ── 5. Zero and copy PT_LOAD segments ─────────────────────────────────────
  if (split_mode) {
    memset(code_buf, 0, code_memsz);
    if (psram_size > 0)
      memset(exec_base, 0, psram_size);
  } else {
    memset(exec_base, 0, image_size);
  }

  for (int i = 0; i < ehdr->e_phnum; i++) {
    const Elf32_Phdr *ph = &phdr_table[i];
    if (ph->p_type != PT_LOAD || ph->p_filesz == 0)
      continue;
    if (ph->p_offset + ph->p_filesz > (uint32_t)file_len) {
      show_error("ELF: segment data out of bounds", NULL);
      goto out;
    }

    const uint8_t *src = file_buf + ph->p_offset;

    if (split_mode && i == code_seg_idx) {
      // Code segment → SRAM (direct write)
      if (ph->p_vaddr - code_vaddr + ph->p_filesz > code_memsz) {
        show_error("ELF: code segment exceeds buffer", NULL);
        goto out;
      }
      memcpy(code_buf + (ph->p_vaddr - code_vaddr), src, ph->p_filesz);
    } else if (split_mode) {
      // Data segment → PSRAM (uncached write)
      uint32_t off = ph->p_vaddr - data_vaddr_start;
      if (off + ph->p_memsz > psram_size) {
        show_error("ELF: data segment exceeds buffer", NULL);
        goto out;
      }
      memcpy(exec_base + off, src, ph->p_filesz);
    } else {
      // Fallback: everything → PSRAM
      if (ph->p_vaddr - mem_min + ph->p_memsz > image_size) {
        show_error("ELF: segment exceeds image", NULL);
        goto out;
      }
      memcpy(exec_base + (ph->p_vaddr - mem_min), src, ph->p_filesz);
    }
  }

  // ── 6. Apply relocations (dual-bias for split mode) ───────────────────────
  //
  // For split mode, each relocated pointer needs the correct bias depending
  // on which segment the *pointed-to* address falls in:
  //   - vaddr in [code_vaddr, code_vend) → code_bias (SRAM address)
  //   - vaddr in [data_vaddr_start, ...)  → data_bias (PSRAM cached address)
  //
  // For fallback mode, a single load_bias covers everything (same as before).

  {
    // Bias for converting vaddrs to runtime addresses
    uint32_t code_bias = split_mode ? (uint32_t)code_buf - code_vaddr : 0;
    uint32_t data_bias = split_mode ? (uint32_t)load_base - data_vaddr_start
                                    : (uint32_t)load_base - mem_min;
    uint32_t fallback_bias = split_mode ? 0 : (uint32_t)load_base - mem_min;

    for (int i = 0; i < ehdr->e_phnum; i++) {
      const Elf32_Phdr *ph = &phdr_table[i];
      if (ph->p_type != PT_DYNAMIC)
        continue;

      // Read PT_DYNAMIC from whichever buffer it lives in
      const Elf32_Dyn *dyn;
      if (split_mode && ph->p_vaddr >= code_vaddr && ph->p_vaddr < code_vend) {
        dyn = (const Elf32_Dyn *)(code_buf + (ph->p_vaddr - code_vaddr));
      } else if (split_mode) {
        dyn = (const Elf32_Dyn *)(exec_base + (ph->p_vaddr - data_vaddr_start));
      } else {
        dyn = (const Elf32_Dyn *)(exec_base + (ph->p_vaddr - mem_min));
      }

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

      // Helper: resolve a vaddr to the write pointer and the appropriate bias
      // for the value being relocated.
      #define RESOLVE_TARGET(vaddr, write_ptr, target_ok) do { \
        if (split_mode) { \
          if ((vaddr) >= code_vaddr && (vaddr) < code_vend) { \
            uint32_t _off = (vaddr) - code_vaddr; \
            if (_off + sizeof(uint32_t) <= code_memsz) { \
              write_ptr = (uint32_t *)(code_buf + _off); \
              target_ok = true; \
            } \
          } else { \
            uint32_t _off = (vaddr) - data_vaddr_start; \
            if (_off + sizeof(uint32_t) <= psram_size) { \
              write_ptr = (uint32_t *)(exec_base + _off); \
              target_ok = true; \
            } \
          } \
        } else { \
          uint32_t _off = (vaddr) - mem_min; \
          if (_off + sizeof(uint32_t) <= image_size) { \
            write_ptr = (uint32_t *)(exec_base + _off); \
            target_ok = true; \
          } \
        } \
      } while(0)

      // Select bias based on which segment a runtime vaddr points into
      #define SELECT_BIAS(pointed_vaddr) \
        (split_mode ? ((pointed_vaddr) >= code_vaddr && (pointed_vaddr) < code_vend \
                       ? code_bias : data_bias) \
                    : fallback_bias)

      // Elf32_Rel — implicit addend: *target += bias
      if (rel_addr && rel_size) {
        const Elf32_Rel *rel;
        if (split_mode && rel_addr >= code_vaddr && rel_addr < code_vend)
          rel = (const Elf32_Rel *)(code_buf + (rel_addr - code_vaddr));
        else if (split_mode)
          rel = (const Elf32_Rel *)(exec_base + (rel_addr - data_vaddr_start));
        else
          rel = (const Elf32_Rel *)(exec_base + (rel_addr - mem_min));

        uint32_t count = rel_size / sizeof(Elf32_Rel);
        for (uint32_t j = 0; j < count; j++) {
          if (ELF32_R_TYPE(rel[j].r_info) == R_ARM_RELATIVE) {
            uint32_t *target = NULL;
            bool target_ok = false;
            RESOLVE_TARGET(rel[j].r_offset, target, target_ok);
            if (!target_ok) continue;
            // *target contains the original vaddr — pick bias based on where it points
            uint32_t pointed_vaddr = *target;
            *target = pointed_vaddr + SELECT_BIAS(pointed_vaddr);
          }
        }
      }

      // Elf32_Rela — explicit addend: *target = bias + r_addend
      if (rela_addr && rela_size) {
        const Elf32_Rela *rela;
        if (split_mode && rela_addr >= code_vaddr && rela_addr < code_vend)
          rela = (const Elf32_Rela *)(code_buf + (rela_addr - code_vaddr));
        else if (split_mode)
          rela = (const Elf32_Rela *)(exec_base + (rela_addr - data_vaddr_start));
        else
          rela = (const Elf32_Rela *)(exec_base + (rela_addr - mem_min));

        uint32_t count = rela_size / sizeof(Elf32_Rela);
        for (uint32_t j = 0; j < count; j++) {
          if (ELF32_R_TYPE(rela[j].r_info) == R_ARM_RELATIVE) {
            uint32_t *target = NULL;
            bool target_ok = false;
            RESOLVE_TARGET(rela[j].r_offset, target, target_ok);
            if (!target_ok) continue;
            uint32_t pointed_vaddr = (uint32_t)rela[j].r_addend;
            *target = pointed_vaddr + SELECT_BIAS(pointed_vaddr);
          }
        }
      }

      #undef RESOLVE_TARGET
      #undef SELECT_BIAS
      break; // Only one PT_DYNAMIC segment expected
    }
  }

  // ── 6a. Free file buffer — no longer needed after relocation ───────────
  // Save e_entry before freeing, since ehdr points into file_buf.
  Elf32_Addr saved_entry = ehdr->e_entry;
  umm_free(file_buf);
  file_buf = NULL;

  // ── 7. Flush XIP cache and compute entry point ──────────────────────────
  __asm volatile ("dsb sy");
  xip_cache_invalidate_all();
  __asm volatile ("isb sy");

  // e_entry may have the Thumb bit set (bit 0 = 1); strip it for the offset
  // calculation, then re-apply for the function pointer call convention.
  uintptr_t entry_voff_raw = saved_entry & ~1u;
  uintptr_t entry_addr;
  if (split_mode && entry_voff_raw >= code_vaddr && entry_voff_raw < code_vend) {
    entry_addr = (uintptr_t)code_buf + (entry_voff_raw - code_vaddr);
  } else if (split_mode) {
    entry_addr = (uintptr_t)load_base + (entry_voff_raw - data_vaddr_start);
  } else {
    uintptr_t entry_voff = entry_voff_raw - mem_min;
    if (entry_voff >= image_size) {
      show_error("ELF: entry point out of bounds", NULL);
      goto out;
    }
    entry_addr = (uintptr_t)load_base + entry_voff;
  }
  entry_addr |= 1u; // Thumb mode

  printf("[NATIVE] Entry %p (thumb%s)\n", (void *)entry_addr,
         split_mode ? ", SRAM" : ", cached PSRAM");

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

  // Resume Core 1 before launching the app.  The pause was only needed to
  // prevent PSRAM heap contention during ELF loading/relocation above.
  // Native apps (e.g. DOOM) register audio callbacks that run on Core 1,
  // so it must be active during app execution.
  g_core1_pause = false;

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
  // ── 9. Cleanup ─────────────────────────────────────────────────────────────
  // Re-pause Core 1 while we free PSRAM buffers to avoid contention,
  // then unpause after cleanup is done.
  g_native_audio_callback = NULL;  // stop app's Core 1 callback before freeing code
  g_core1_pause = true;
  sleep_ms(10);  // let any in-flight Core 1 iteration finish
  audio_stop_stream();   // ensure audio streaming is stopped on exit/crash
  audio_stop_tone();
  if (code_buf)
    free(code_buf);
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
