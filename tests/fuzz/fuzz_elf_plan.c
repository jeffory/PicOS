// libFuzzer target for src/os/elf_plan.c: plan, copy and relocate an
// arbitrary byte string exactly as the loaders do.  Any out-of-bounds read
// or write (ASan) or UB (UBSan) is a crash.  Seed corpus: tests/fuzz/corpus/
// elf_plan (real PicoDeck app ELFs).
#include "elf_plan.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_MAX_IMAGE (1u << 20)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > 0xFFFFFFFFu)
    return 0;
  uint32_t len = (uint32_t)size;
  elf_plan_t plan;
  if (elf_plan_header(data, len, len, &plan) != ELF_OK)
    return 0;
  // The loaders read the phdr table into its own allocation.
  uint8_t *ph = malloc(plan.phdrs_size);
  memcpy(ph, data + plan.phoff, plan.phdrs_size);
  if (elf_plan_segments(&plan, ph, plan.phdrs_size, len, FUZZ_MAX_IMAGE) == ELF_OK) {
    uint8_t *img = calloc(1, plan.image_size);
    for (uint16_t i = 0; i < plan.phnum; i++) {
      elf32_phdr_t s = elf_plan_phdr(ph, i);
      if (s.p_type == ELF_PT_LOAD && s.p_filesz)
        memcpy(img + (s.p_vaddr - plan.mem_min), data + s.p_offset, s.p_filesz);
    }
    // One region (simulator / firmware all-PSRAM) ...
    elf_region_t r1 = {plan.mem_min, plan.image_size, img, 0x11000000u};
    elf_relocate(&plan, &r1, 1, NULL);
    // ... and the firmware's split layout when the plan allows it.
    if (plan.code_first && plan.code_memsz <= plan.image_size) {
      uint32_t data_start = plan.code_vaddr + plan.code_memsz;
      elf_region_t r2[2] = {
          {plan.code_vaddr, plan.code_memsz, img, 0x20001000u},
          {data_start, plan.image_size - plan.code_memsz,
           img + plan.code_memsz, 0x11000000u}};
      elf_relocate(&plan, r2, 2, NULL);
    }
    free(img);
  }
  free(ph);
  return 0;
}
