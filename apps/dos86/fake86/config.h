/*
  Fake86: A portable, open-source 8086 PC emulator.
  Copyright (C)2010-2013 Mike Chambers
  Adapted for PicOS/dos86 — stripped of SDL, audio, video, disk dependencies.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.
*/

#ifndef FAKE86_CONFIG_H
#define FAKE86_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ---- CPU model selection (pick exactly one) ---- */
/* V20 = NEC V20/V30, a superset of 80186 instructions */
#define CPU_V20

#if defined(CPU_8086)
  #define CPU_CLEAR_ZF_ON_MUL
  #define CPU_ALLOW_POP_CS
#else
  #define CPU_ALLOW_ILLEGAL_OP_EXCEPTION
  #define CPU_LIMIT_SHIFT_COUNT
#endif

#if defined(CPU_V20)
  #define CPU_NO_SALC
#endif

/* 8086/V20 style PUSH SP (not 286 style) */
#define CPU_SET_HIGH_FLAGS

/* ---- Memory sizes ---- */
#define RAM_SIZE    0x100000   /* 1 MB conventional + UMA */
#define VRAM_SIZE   0x40000    /* 256 KB video RAM */
#define PORT_SIZE   0x10000    /* 64 KB I/O port space */

/* ---- External memory pointers (set by main.c) ---- */
extern uint8_t *g_ram;        /* 1 MB: 0x00000-0xFFFFF */
extern uint8_t *g_vram;       /* 256 KB: mapped at 0xA0000-0xBFFFF */
extern uint8_t *g_portram;    /* 64 KB: I/O port shadow */

/* ---- ROM readonly check (replaces upstream's 1MB readonly[] array) ---- */
static inline bool is_readonly(uint32_t addr) {
    return (addr >= 0xF0000 && addr <= 0xFFFFF) ||  /* BIOS ROM area */
           (addr >= 0xC0000 && addr <= 0xC7FFF);    /* Video BIOS ROM */
}

/* ---- Timing interval (instruction count between timing callbacks) ---- */
#define TIMING_INTERVAL  15

/* Called by intcall86 for video interrupt (INT 10h) */
extern void vidinterrupt(void);

/* Disable features we don't need on PicOS */
/* #define NETWORKING_ENABLED */
/* #define NETWORKING_OLDCARD */
/* #define DISK_CONTROLLER_ATA */
/* #define CPU_ADDR_MODE_CACHE */
/* #define USE_PREFETCH_QUEUE */
/* #define BENCHMARK_BIOS */

#endif /* FAKE86_CONFIG_H */
