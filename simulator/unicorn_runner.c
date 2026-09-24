// unicorn_runner.c — Unicorn Engine integration for running ARM ELF native apps
// in the PicOS simulator. Loads ELF32 PIE binaries, sets up emulated memory
// regions, wires the PicoCalcAPI struct with trampoline-based function pointers,
// and dispatches API calls to host simulator functions.

#include "unicorn_runner.h"
#include <unicorn/unicorn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// PicOS includes
#include "os.h"
#include "elf_plan.h"
#include "crashlog.h"
#include "sim_hooks.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "hal/hal_sdcard.h"
#include "hal/hal_timing.h"

// =============================================================================
// Memory map constants
// =============================================================================

#define EMU_CODE_BASE       0x10000000u
#define EMU_CODE_SIZE       (8u * 1024 * 1024)   // 8MB (matches PSRAM on hardware)

#define EMU_DATA_BASE       0x20000000u
#define EMU_DATA_SIZE       (4u * 1024 * 1024)   // 4MB

#define EMU_STACK_BASE      0x30000000u
#define EMU_STACK_SIZE      (256u * 1024)         // 256KB

#define EMU_HEAP_BASE       0x40000000u
#define EMU_HEAP_SIZE       (4u * 1024 * 1024)    // 4MB

#define EMU_ARENA_BASE      0x50000000u
#define EMU_ARENA_SIZE      (64u * 1024)          // 64KB

#define EMU_FB_BASE         0xD0000000u
#define EMU_FB_SIZE         (320 * 320 * 2)       // ~200KB, round up to page

#define EMU_API_BASE        0xE0000000u
#define EMU_API_SIZE        (64u * 1024)          // 64KB for g_api + sub-tables

#define EMU_TRAMP_BASE      0xF0000000u
#define EMU_TRAMP_SIZE      (4u * 1024)           // 4KB for trampolines

#define EMU_REVCALL_BASE    0xF0010000u
#define EMU_REVCALL_SIZE    (4u * 1024)           // 4KB for reverse-call stubs

// Maximum number of trampoline slots (API functions)
#define MAX_TRAMP_SLOTS     512

// Flag set when uc_emu_stop was called during nested emulation (SVC #254).
// The outer emulation loop checks this to know it should restart.
int g_emu_nested_stop = 0;

// =============================================================================
// ELF validation/relocation: src/os/elf_plan.c (shared with the firmware loader)
// =============================================================================

// =============================================================================
// Handle table: maps 32-bit emulated handles <-> 64-bit host pointers
// =============================================================================

#define MAX_HANDLES 256

static void *s_handle_to_host[MAX_HANDLES];
static uint32_t s_next_handle = 1;  // 0 = NULL

uint32_t handle_wrap(void *host_ptr) {
    if (!host_ptr) return 0;
    // Search for existing mapping first
    for (uint32_t i = 1; i < MAX_HANDLES; i++) {
        if (s_handle_to_host[i] == host_ptr) return i;
    }
    // Scan for a free slot starting from s_next_handle
    for (int i = 0; i < MAX_HANDLES; i++) {
        uint32_t idx = (s_next_handle + i) % MAX_HANDLES;
        if (idx == 0) idx = 1;  // skip slot 0 (reserved for NULL)
        if (s_handle_to_host[idx] == NULL) {
            s_handle_to_host[idx] = host_ptr;
            s_next_handle = idx + 1;
            return idx;
        }
    }
    fprintf(stderr, "[UNICORN] Handle table exhausted!\n");
    return 0;
}

void *handle_unwrap(uint32_t handle) {
    if (handle == 0 || handle >= MAX_HANDLES) return NULL;
    return s_handle_to_host[handle];
}

void handle_free(uint32_t handle) {
    if (handle > 0 && handle < MAX_HANDLES) {
        s_handle_to_host[handle] = NULL;
    }
}

static void handle_reset(void) {
    memset(s_handle_to_host, 0, sizeof(s_handle_to_host));
    s_next_handle = 1;
}

// =============================================================================
// String arena: bump allocator for returning strings to emulated code
// =============================================================================

static uint32_t s_arena_offset = 0;
static uint32_t s_arena_reserved = 0;  // watermark: protect startup strings from wrap-around

uint32_t arena_write_string(uc_engine *uc, const char *str) {
    if (!str) return 0;
    uint32_t len = (uint32_t)strlen(str) + 1;
    if (s_arena_offset + len > EMU_ARENA_SIZE) {
        // Wrap around, but skip past the reserved startup strings
        s_arena_offset = s_arena_reserved;
        if (s_arena_offset + len > EMU_ARENA_SIZE) return 0;
    }
    uint32_t addr = EMU_ARENA_BASE + s_arena_offset;
    uc_mem_write(uc, addr, str, len);
    s_arena_offset += (len + 3) & ~3u;  // Align to 4 bytes
    return addr;
}

uint32_t arena_write_buffer(uc_engine *uc, const void *data, uint32_t len) {
    if (!data || len == 0) return 0;
    if (s_arena_offset + len > EMU_ARENA_SIZE) {
        s_arena_offset = s_arena_reserved;
        if (s_arena_offset + len > EMU_ARENA_SIZE) return 0;
    }
    uint32_t addr = EMU_ARENA_BASE + s_arena_offset;
    uc_mem_write(uc, addr, data, len);
    s_arena_offset += (len + 3) & ~3u;
    return addr;
}

// =============================================================================
// Heap emulator: simple bump allocator for qmiAlloc/qmiFree
// =============================================================================

static uint32_t s_heap_offset = 0;

uint32_t heap_alloc(uint32_t size) {
    size = (size + 7) & ~7u;  // 8-byte align
    if (s_heap_offset + size > EMU_HEAP_SIZE) {
        fprintf(stderr, "[UNICORN] Heap exhausted!\n");
        return 0;
    }
    uint32_t addr = EMU_HEAP_BASE + s_heap_offset;
    s_heap_offset += size;
    return addr;
}

void heap_free(uint32_t addr) {
    // Simple bump allocator doesn't free. Could be improved later.
    (void)addr;
}

// =============================================================================
// Helper: read NUL-terminated string from emulated memory
// =============================================================================

static char s_str_buf[4][1024];
static int s_str_idx = 0;

char *uc_read_string(uc_engine *uc, uint32_t addr) {
    if (addr == 0) return NULL;
    char *buf = s_str_buf[s_str_idx];
    s_str_idx = (s_str_idx + 1) & 3;
    // Read in small chunks to avoid crossing memory region boundaries.
    // A bulk uc_mem_read that spans past a mapped region zeroes the buffer.
    size_t max = sizeof(s_str_buf[0]) - 1;
    size_t pos = 0;
    while (pos < max) {
        size_t chunk = 64;
        if (pos + chunk > max) chunk = max - pos;
        uc_err err = uc_mem_read(uc, addr + pos, &buf[pos], chunk);
        if (err != UC_ERR_OK) {
            buf[pos] = '\0';
            return buf;
        }
        // Check for NUL within this chunk
        for (size_t i = pos; i < pos + chunk; i++) {
            if (buf[i] == '\0') return buf;
        }
        pos += chunk;
    }
    buf[max] = '\0';
    return buf;
}

// =============================================================================
// Trampoline dispatch system
// =============================================================================

// Forward declarations for trampoline handlers (defined in unicorn_trampolines.c)
extern void unicorn_tramp_init(uc_engine *uc);
extern void unicorn_tramp_dispatch(uc_engine *uc, uint32_t slot);
extern uint32_t unicorn_tramp_get_slot_count(void);

// Global Unicorn engine pointer (used by trampolines)
uc_engine *g_uc = NULL;

// Interrupt hook for SVC-based trampolines.
// On Cortex-M, SVC generates exception #11 (SVCall). Unicorn passes the
// interrupt number via the `intno` parameter.
static void trampoline_hook(uc_engine *uc, uint32_t intno, void *user_data) {
    (void)user_data;

    // SVC on ARM: Unicorn reports intno=2 (QEMU convention) or intno=3.
    // We need to read the SVC number from the instruction itself.
    // The PC points to the instruction AFTER the SVC, so read PC-2.
    if (intno == 2 || intno == 3) {
        uint32_t pc;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        // In Unicorn's interrupt hook, PC points AT the triggering instruction.
        uint16_t insn;
        uc_mem_read(uc, pc, &insn, 2);

        // Only handle SVC instructions (0xDFxx).
        // PC=0 with insn=0 means the app returned (LR was set to 0).
        if ((insn & 0xFF00) != 0xDF00) {
            if (pc == 0) {
                printf("[UNICORN] Normal exit: app returned from picos_main()\n");
            } else {
                fprintf(stderr, "[UNICORN] Unexpected interrupt on non-SVC insn 0x%04x at 0x%08x\n",
                        insn, pc);
            }
            uc_emu_stop(uc);
            return;
        }

        // SVC #254 at REVCALL_BASE: nested callback return sentinel.
        // (Checked by address, not SVC number, so slot 254 stays usable.)
        if (pc == EMU_REVCALL_BASE) {
            g_emu_nested_stop = 1;
            uc_emu_stop(uc);
            return;
        }

        // Derive the slot from the PC, not the SVC immediate: SVC #imm8
        // wraps at 256, but slot count has grown past that.
        if (pc < EMU_TRAMP_BASE || pc >= EMU_TRAMP_BASE + MAX_TRAMP_SLOTS * 4) {
            fprintf(stderr, "[UNICORN] SVC outside trampoline region at 0x%08x\n", pc);
            uc_emu_stop(uc);
            return;
        }
        uint32_t slot = (pc - EMU_TRAMP_BASE) / 4;

        // Dispatch the API call
        unicorn_tramp_dispatch(uc, slot);

        // Return to caller: read LR (set by the BLX that called the trampoline)
        // and write it to PC.  Strip Thumb bit (bit 0) since PC doesn't encode it.
        uint32_t lr;
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        uc_reg_write(uc, UC_ARM_REG_PC, &lr);
    } else {
        fprintf(stderr, "[UNICORN] Unhandled interrupt: %u at PC=", intno);
        uint32_t pc;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        fprintf(stderr, "0x%08x\n", pc);
        uc_emu_stop(uc);
    }
}

// =============================================================================
// ELF loader
// =============================================================================

// Name of the app being loaded, for load_refused().
static const char *s_loading_app_name = NULL;

// A load failure, reported like the firmware loader's show_error()
// (native_loader.c): an "[UNICORN] ELF rejected" line on stderr, a
// NATIVE ERROR entry in /system/error.log, and the test channel's outcome
// (app.exited result "load_failed", error "<reason>[: <detail>]").
static void load_refused(const char *reason, const char *detail) {
    char msg[160];
    if (detail && detail[0])
        snprintf(msg, sizeof(msg), "%s: %s", reason, detail);
    else
        snprintf(msg, sizeof(msg), "%s", reason);
    fprintf(stderr, "[UNICORN] ELF rejected: %s\n", msg);
    crashlog_write("NATIVE ERROR", s_loading_app_name, reason, detail);
    sim_app_outcome_set(SIM_APP_RESULT_LOAD_FAILED, msg);
    sim_log_err("[NATIVE] %s: %s", s_loading_app_name ? s_loading_app_name : "?", msg);
}

static bool load_elf(uc_engine *uc, const char *path, uint32_t *out_entry) {
    // Resolve virtual SD card path to host filesystem path
    char full_path[1024];
    if (!hal_sdcard_resolve(path, full_path, sizeof(full_path))) {
        load_refused("ELF: cannot open file", path);
        return false;
    }

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        fprintf(stderr, "[UNICORN] Failed to open ELF: %s (resolved: %s)\n", path, full_path);
        load_refused("ELF: cannot open file", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_len < 0 || file_len > 0x7FFFFFFFL) file_len = 0;

    // Same validation as the firmware loader (elf_plan.c): a malformed image
    // is refused here, before anything reaches Unicorn.
    uint8_t ehdr_buf[ELF_EHDR_SIZE];
    size_t got = fread(ehdr_buf, 1, sizeof(ehdr_buf), f);
    elf_plan_t plan;
    elf_err_t eerr = elf_plan_header(ehdr_buf, (uint32_t)got,
                                     (uint32_t)file_len, &plan);
    if (eerr != ELF_OK) {
        load_refused(elf_strerror(eerr), NULL);
        fclose(f);
        return false;
    }

    uint8_t *phdrs = (uint8_t *)malloc(plan.phdrs_size);
    if (!phdrs) {
        load_refused("ELF: out of memory for phdr", NULL);
        fclose(f);
        return false;
    }
    if (fseek(f, (long)plan.phoff, SEEK_SET) != 0 ||
        fread(phdrs, plan.phdrs_size, 1, f) != 1) {
        load_refused("ELF: failed to read phdr table", NULL);
        free(phdrs);
        fclose(f);
        return false;
    }

    eerr = elf_plan_segments(&plan, phdrs, plan.phdrs_size,
                             (uint32_t)file_len, EMU_CODE_SIZE);
    if (eerr != ELF_OK) {
        char detail[48] = "";
        if (eerr == ELF_ERR_IMAGE_TOO_LARGE)
            snprintf(detail, sizeof(detail), "> %u byte code region",
                     (uint32_t)EMU_CODE_SIZE);
        load_refused(elf_strerror(eerr), detail[0] ? detail : NULL);
        free(phdrs);
        fclose(f);
        return false;
    }

    uint32_t mem_min = plan.mem_min;
    uint32_t image_size = plan.image_size;
    printf("[UNICORN] ELF image: %u bytes (vaddr 0x%08x..0x%08x)\n",
           image_size, mem_min, plan.mem_max);

    // Allocate a host-side buffer to build the image, then write it all at once
    uint8_t *image_buf = (uint8_t *)calloc(1, image_size);
    if (!image_buf) {
        load_refused("ELF: out of memory for image", NULL);
        free(phdrs);
        fclose(f);
        return false;
    }

    // Copy PT_LOAD segments into the buffer (bounds proven by elf_plan)
    for (uint16_t i = 0; i < plan.phnum; i++) {
        elf32_phdr_t ph = elf_plan_phdr(phdrs, i);
        if (ph.p_type != ELF_PT_LOAD || ph.p_filesz == 0) continue;
        fseek(f, (long)ph.p_offset, SEEK_SET);
        size_t off = ph.p_vaddr - mem_min;
        if (fread(image_buf + off, ph.p_filesz, 1, f) != 1) {
            load_refused("ELF: failed to read segment", NULL);
            free(image_buf);
            free(phdrs);
            fclose(f);
            return false;
        }
    }

    // Apply R_ARM_RELATIVE relocations: the image runs at EMU_CODE_BASE.
    elf_region_t region = {mem_min, image_size, image_buf, EMU_CODE_BASE};
    elf_reloc_stats_t rstats;
    eerr = elf_relocate(&plan, &region, 1, &rstats);
    if (eerr != ELF_OK) {
        load_refused(elf_strerror(eerr), NULL);
        free(image_buf);
        free(phdrs);
        fclose(f);
        return false;
    }

    // Write the relocated image into Unicorn memory
    uc_err err = uc_mem_write(uc, EMU_CODE_BASE, image_buf, image_size);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "[UNICORN] Failed to write ELF image: %s\n", uc_strerror(err));
        free(image_buf);
        free(phdrs);
        fclose(f);
        return false;
    }

    // Map the ELF's original virtual address range (0x0..image_size) as a shadow
    // region.  Newlib and other library code may hold unrelocated pointers (e.g.
    // internal _reent function pointers, lock stubs) that reference the original
    // VirtAddr space.  Without this mapping, every 4KB page fault triggers the
    // mem_error_hook one at a time, adding hundreds of dynamic uc_mem_map calls.
    // On real RP2350 hardware, low addresses land in boot ROM (reads succeed,
    // writes are silently ignored), so this mirrors that behavior.
    if (mem_min == 0 && image_size > 0) {
        uint32_t shadow_size = (image_size + 0xFFF) & ~0xFFFu;
        uc_err serr = uc_mem_map(uc, 0, shadow_size, UC_PROT_ALL);
        if (serr == UC_ERR_OK) {
            printf("[UNICORN] Mapped shadow region 0x00000000..0x%08x for unrelocated accesses\n",
                   shadow_size);
        } else {
            fprintf(stderr, "[UNICORN] WARNING: shadow region map failed: %s (size=0x%08x)\n",
                    uc_strerror(serr), shadow_size);
        }
        // Non-fatal if it fails (mem_error_hook will handle individual pages)
    }

    // Compute entry point (inside the image, checked by elf_plan)
    *out_entry = EMU_CODE_BASE + plan.entry_off;
    // Set Thumb bit
    *out_entry |= 1u;

    printf("[UNICORN] Entry point: 0x%08x\n", *out_entry);

    free(image_buf);
    free(phdrs);
    fclose(f);
    return true;
}

// =============================================================================
// Build emulated g_api struct in Unicorn memory
// =============================================================================

// These are defined in unicorn_trampolines.c
extern void unicorn_build_api_struct(uc_engine *uc, uint32_t api_base, uint32_t tramp_base);

// =============================================================================
// Main entry point
// Memory error hook — prints the faulting address and instruction context
static bool mem_error_hook(uc_engine *uc, uc_mem_type type,
                           uint64_t address, int size, int64_t value,
                           void *user_data) {
    (void)user_data; (void)value;
    uint32_t pc, sp, lr;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    const char *op = (type == UC_MEM_READ_UNMAPPED) ? "READ" :
                     (type == UC_MEM_WRITE_UNMAPPED) ? "WRITE" : "FETCH";
    fprintf(stderr, "[UNICORN] MEM_ERROR: %s unmapped addr=0x%08llx size=%d at PC=0x%08x SP=0x%08x LR=0x%08x\n",
            op, (unsigned long long)address, size, pc, sp, lr);
    fflush(stderr);

    // Dynamically map the faulting page so emulation can continue.
    // This handles NULL pointer dereferences and hardware register accesses
    // (e.g. RP2350 watchdog poke) that don't exist in the simulator.
    uint64_t page_base = address & ~0xFFFULL;
    uint32_t page_size = 0x1000;
    uc_err err = uc_mem_map(uc, page_base, page_size, UC_PROT_ALL);
    if (err == UC_ERR_OK) {
        return true;  // retry the access
    }
    // If we can't map it, the access fails and emulation stops
    return false;
}

// =============================================================================

bool unicorn_run_app(const char *elf_path, const char *app_dir,
                     const char *app_id, const char *app_name) {
    printf("[UNICORN] Loading native app: %s\n", elf_path);
    s_loading_app_name = app_name;

    uc_engine *uc;
    uc_err err;

    // Create Unicorn instance (ARM Thumb mode, Cortex-M33)
    err = uc_open(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS, &uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "[UNICORN] uc_open failed: %s\n", uc_strerror(err));
        return false;
    }

    g_uc = uc;

    // Reset state
    handle_reset();
    s_arena_offset = 0;
    s_arena_reserved = 0;
    s_heap_offset = 0;

    // ── Map memory regions ──────────────────────────────────────────────────

    // Code region
    err = uc_mem_map(uc, EMU_CODE_BASE, EMU_CODE_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "[UNICORN] Failed to map code region: %s\n", uc_strerror(err));
        goto fail;
    }

    // Data region (for large ELFs that have data segments far from code)
    err = uc_mem_map(uc, EMU_DATA_BASE, EMU_DATA_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "[UNICORN] Failed to map data region: %s\n", uc_strerror(err));
        goto fail;
    }

    // Stack (grows down from top)
    err = uc_mem_map(uc, EMU_STACK_BASE, EMU_STACK_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "[UNICORN] Failed to map stack: %s\n", uc_strerror(err));
        goto fail;
    }

    // Heap
    err = uc_mem_map(uc, EMU_HEAP_BASE, EMU_HEAP_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "[UNICORN] Failed to map heap: %s\n", uc_strerror(err));
        goto fail;
    }

    // String arena
    err = uc_mem_map(uc, EMU_ARENA_BASE, EMU_ARENA_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "[UNICORN] Failed to map arena: %s\n", uc_strerror(err));
        goto fail;
    }

    // Framebuffer — use Unicorn-managed memory so emulated app writes stay in
    // the emulated address space.  The flush trampoline copies data out via
    // uc_mem_read() into the simulator's back buffer before presenting.
    {
        uint32_t fb_map_size = (EMU_FB_SIZE + 0xFFF) & ~0xFFFu;
        err = uc_mem_map(uc, EMU_FB_BASE, fb_map_size, UC_PROT_ALL);
        if (err != UC_ERR_OK) {
            fprintf(stderr, "[UNICORN] Failed to map framebuffer: %s\n", uc_strerror(err));
            goto fail;
        }
    }

    // API struct region
    err = uc_mem_map(uc, EMU_API_BASE, EMU_API_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "[UNICORN] Failed to map API region: %s\n", uc_strerror(err));
        goto fail;
    }

    // Trampoline region
    err = uc_mem_map(uc, EMU_TRAMP_BASE, EMU_TRAMP_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "[UNICORN] Failed to map trampoline region: %s\n", uc_strerror(err));
        goto fail;
    }

    // Reverse-call stubs region — used as return address for nested uc_emu_start callbacks.
    // Write a NOP at REVCALL_BASE so Unicorn's `until` address check fires before executing.
    err = uc_mem_map(uc, EMU_REVCALL_BASE, EMU_REVCALL_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "[UNICORN] Failed to map reverse-call region: %s\n", uc_strerror(err));
        goto fail;
    }
    {
        // Write SVC #254 + BX LR at REVCALL_BASE. When a nested callback
        // returns (BX LR → REVCALL_BASE), it executes SVC #254 which the
        // interrupt hook recognizes as "end of nested callback" and calls
        // uc_emu_stop. The global stop flag is acceptable here because
        // the trampoline code that invoked uc_emu_start expects it.
        uint16_t revcall_code[2] = {
            0xDFFE,  // SVC #254
            0x4770,  // BX LR (safety, shouldn't reach here)
        };
        uc_mem_write(uc, EMU_REVCALL_BASE, revcall_code, sizeof(revcall_code));
    }

    // ── Fill trampoline region with SVC #N + BX LR instructions ────────
    // Each trampoline slot is 4 bytes: SVC #slot (2 bytes) + BX LR (2 bytes)
    // The SVC triggers an interrupt which we intercept via UC_HOOK_INTR.
    // The interrupt hook dispatches the API call, then BX LR returns to caller.
    {
        for (uint32_t i = 0; i < MAX_TRAMP_SLOTS; i++) {
            // SVC #imm8: encoding = 0xDF00 | (imm8 & 0xFF). The immediate
            // wraps past 255, but the hook derives the slot from the PC,
            // so any slot count works — the encoding just needs to be an
            // SVC. Keep imm = slot & 0xFF for debug readability.
            uint16_t svc = 0xDF00 | (i & 0xFF);  // SVC #i
            uint16_t bx_lr = 0x4770;              // BX LR
            uc_mem_write(uc, EMU_TRAMP_BASE + i * 4, &svc, 2);
            uc_mem_write(uc, EMU_TRAMP_BASE + i * 4 + 2, &bx_lr, 2);
        }
    }

    // ── Set up interrupt hook for SVC-based trampolines ─────────────────
    {
        uc_hook hook;
        err = uc_hook_add(uc, &hook, UC_HOOK_INTR,
                          (void *)trampoline_hook, NULL,
                          1, 0);  // range ignored for INTR hooks
        if (err != UC_ERR_OK) {
            fprintf(stderr, "[UNICORN] Failed to add interrupt hook: %s\n", uc_strerror(err));
            goto fail;
        }
    }


    // ── Memory error hook for debugging ────────────────────────────────
    {
        uc_hook mem_hook;
        err = uc_hook_add(uc, &mem_hook,
                          UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED,
                          (void *)mem_error_hook, NULL, 1, 0);
        if (err != UC_ERR_OK) {
            fprintf(stderr, "[UNICORN] Failed to add memory hook: %s\n", uc_strerror(err));
        }
    }

    // ── Initialize trampolines ──────────────────────────────────────────
    unicorn_tramp_init(uc);

    // ── Build the emulated API struct ───────────────────────────────────
    unicorn_build_api_struct(uc, EMU_API_BASE, EMU_TRAMP_BASE);

    // ── Load the ELF ────────────────────────────────────────────────────
    uint32_t entry_point;
    if (!load_elf(uc, elf_path, &entry_point)) {
        fprintf(stderr, "[UNICORN] Failed to load ELF\n");
        goto fail;
    }

    // ── Write entry strings into the arena ──────────────────────────────
    uint32_t emu_app_dir  = arena_write_string(uc, app_dir);
    uint32_t emu_app_id   = arena_write_string(uc, app_id);
    uint32_t emu_app_name = arena_write_string(uc, app_name);
    s_arena_reserved = s_arena_offset;  // protect startup strings from wrap-around

    // ── Set up registers ────────────────────────────────────────────────
    // picos_main(const PicoCalcAPI *api, const char *app_dir,
    //            const char *app_id, const char *app_name)
    uint32_t sp = EMU_STACK_BASE + EMU_STACK_SIZE;  // Stack top
    uint32_t r0 = EMU_API_BASE;     // api pointer
    uint32_t r1 = emu_app_dir;      // app_dir
    uint32_t r2 = emu_app_id;       // app_id
    uint32_t r3 = emu_app_name;     // app_name

    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    uc_reg_write(uc, UC_ARM_REG_R0, &r0);
    uc_reg_write(uc, UC_ARM_REG_R1, &r1);
    uc_reg_write(uc, UC_ARM_REG_R2, &r2);
    uc_reg_write(uc, UC_ARM_REG_R3, &r3);

    // Set LR to 0 so return from picos_main causes a fetch fault (stopping emulation)
    uint32_t lr = 0;
    uc_reg_write(uc, UC_ARM_REG_LR, &lr);

    printf("[UNICORN] Starting emulation at 0x%08x (SP=0x%08x)\n", entry_point, sp);
    fflush(stdout);  // ensure output visible before emulation starts

    // ── Run ─────────────────────────────────────────────────────────────
    // The main emulation loop must restart when uc_emu_stop is called from
    // within a nested callback (SVC #254). The trampoline handler sets
    // g_emu_nest_depth > 0 during nested calls; if the outer emu_start exits
    // while nested, we resume from the current PC.
    {
        uint32_t start_addr = entry_point;
        for (;;) {
            err = uc_emu_start(uc, start_addr, 0, 0, 0);
            uint32_t pc, sp_val, lr_val;
            uc_reg_read(uc, UC_ARM_REG_PC, &pc);
            uc_reg_read(uc, UC_ARM_REG_SP, &sp_val);
            uc_reg_read(uc, UC_ARM_REG_LR, &lr_val);

            if (err == UC_ERR_OK && g_emu_nested_stop) {
                // Outer emulation was killed by nested uc_emu_stop (SVC #254).
                // Resume from current PC (the SVC dispatch already set PC
                // to LR, so we continue after the trampoline call).
                g_emu_nested_stop = 0;
                start_addr = pc | 1u;  // Thumb bit
                continue;
            }

            printf("[UNICORN] emu_start returned: err=%d pc=0x%08x sp=0x%08x lr=0x%08x nested=%d\n",
                   err, pc, sp_val, lr_val, g_emu_nested_stop);
            fflush(stdout);
            if (err == UC_ERR_OK) {
                printf("[UNICORN] Emulation stopped (uc_emu_stop called). PC=0x%08x SP=0x%08x LR=0x%08x\n",
                       pc, sp_val, lr_val);
            } else if (err == UC_ERR_FETCH_UNMAPPED && pc == 0) {
                printf("[UNICORN] App returned normally\n");
            } else {
                fprintf(stderr, "[UNICORN] Emulation error: %s (PC=0x%08x SP=0x%08x LR=0x%08x)\n",
                        uc_strerror(err), pc, sp_val, lr_val);
            }
            fflush(stdout); fflush(stderr);
            break;
        }
    }

    printf("[UNICORN] App '%s' finished\n", app_name);

    uc_close(uc);
    g_uc = NULL;
    return true;

fail:
    uc_close(uc);
    g_uc = NULL;
    return false;
}
