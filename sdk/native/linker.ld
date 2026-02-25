/* PicOS Native App Linker Script
 *
 * Produces a Position-Independent ELF32 (ET_DYN) for ARM Thumb-2.
 * The OS loads this into PSRAM at a runtime address and applies
 * R_ARM_RELATIVE relocations before calling picos_main.
 *
 * Usage:
 *   arm-none-eabi-gcc ... -T linker.ld -Wl,-pie -Wl,--entry=picos_main
 *
 * Explicit PHDRS block: suppresses the auto-generated PT_PHDR entry
 * that would otherwise cause "PHDR segment not covered by LOAD segment"
 * errors when there is no OS dynamic linker to map the headers.
 */

ENTRY(picos_main)

PHDRS {
    load PT_LOAD FLAGS(7);    /* rwx — one segment; permissions not enforced in PSRAM */
    dyn  PT_DYNAMIC FLAGS(6); /* rw  — dynamic section view into the same region  */
}

SECTIONS {
    . = 0;

    /* ── Code ──────────────────────────────────────────────────────────── */
    .text : {
        *(.text .text.*)
        . = ALIGN(4);
    } :load

    /* ── Read-only data ─────────────────────────────────────────────────── */
    .rodata : {
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } :load

    /* ── Dynamic relocation tables ───────────────────────────────────────
     * Placed before .dynamic so DT_REL / DT_RELA virtual addresses are
     * within the LOAD segment and our ELF loader can find them.          */
    .rel.dyn  : { *(.rel.dyn)  } :load
    .rela.dyn : { *(.rela.dyn) } :load

    /* ── Global Offset Table ─────────────────────────────────────────────
     * Written to by the loader when applying R_ARM_RELATIVE relocations. */
    .got : {
        *(.got)
        *(.got.plt)
        . = ALIGN(4);
    } :load

    /* ── Dynamic section ─────────────────────────────────────────────────
     * Carries DT_REL / DT_RELA / DT_RELASZ entries that our loader reads. */
    .dynamic : { *(.dynamic) } :load :dyn

    /* ── Initialised data ────────────────────────────────────────────────── */
    .data : {
        *(.data .data.*)
        . = ALIGN(4);
    } :load

    /* ── Zero-initialised data ───────────────────────────────────────────
     * p_filesz stops here; p_memsz includes .bss.  The OS zeroes the gap. */
    .bss : {
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
    } :load

    /* ── Discard unreferenced metadata ──────────────────────────────────── */
    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.ARM.attributes)
        *(.ARM.exidx* .ARM.extab*)
        *(.debug_*)
    }
}
