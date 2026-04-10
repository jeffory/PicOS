/*
  DOS86 — VGA register emulation and BIOS INT 10h handler.
*/

#pragma once
#include "config.h"

typedef struct {
    uint8_t sc[0x100];    /* Sequence Controller registers */
    uint8_t crtc[0x100];  /* CRT Controller registers */
    uint8_t attr[0x100];  /* Attribute Controller registers */
    uint8_t gc[0x100];    /* Graphics Controller registers */
    uint8_t dac[768];     /* DAC palette (256 colors x R,G,B) */
    uint8_t dac_index;    /* Current DAC write index */
    uint8_t dac_rgb_step; /* 0=R, 1=G, 2=B */
    uint8_t dac_read_idx;
    uint8_t dac_read_step;
    uint8_t misc_output;
    uint8_t sc_index;
    uint8_t crtc_index;
    uint8_t attr_index;
    uint8_t gc_index;
    bool    attr_flipflop; /* Address/data toggle for attribute controller */
    uint8_t video_mode;    /* Current BIOS video mode */
    uint8_t cursor_start;  /* Cursor start scanline */
    uint8_t cursor_end;    /* Cursor end scanline */
    uint16_t cursor_pos;   /* Cursor position (offset in video memory) */
    uint8_t active_page;   /* Active display page */
    bool    dirty;         /* Set when VRAM or registers change */
} video_state_t;

extern video_state_t video;

void video_reset(void);
uint8_t video_portin(uint16_t port);
void video_portout(uint16_t port, uint8_t val);
void video_int10h(void);   /* BIOS INT 10h handler */
void video_set_dirty(void);
bool video_is_dirty(void);
void video_clear_dirty(void);
