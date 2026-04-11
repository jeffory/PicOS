/*
  DOS86 — VGA register emulation and BIOS INT 10h handler.
  Handles ports 0x3B0-0x3DA and provides INT 10h video services.
*/

#include "video.h"
#include "cpu.h"
#include <string.h>

video_state_t video;

/* Vertical retrace toggle — simulates vblank/hblank timing */
static uint8_t s_retrace_toggle = 0;

void video_reset(void) {
    memset(&video, 0, sizeof(video));
    video.video_mode = 0x03;   /* Default: 80x25 color text */
    video.cursor_start = 6;
    video.cursor_end = 7;
    video.misc_output = 0x63;  /* Color mode, odd/even, clock select */
    video.dirty = true;

    /* Set up default EGA/VGA attribute palette (identity map) */
    for (int i = 0; i < 16; i++) {
        video.attr[i] = (uint8_t)i;
    }

    /* Set up default VGA DAC palette — standard 16-color EGA palette */
    static const uint8_t default_ega_dac[16][3] = {
        { 0,  0,  0}, { 0,  0, 42}, { 0, 42,  0}, { 0, 42, 42},
        {42,  0,  0}, {42,  0, 42}, {42, 21,  0}, {42, 42, 42},
        {21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63},
        {63, 21, 21}, {63, 21, 63}, {63, 63, 21}, {63, 63, 63}
    };
    for (int i = 0; i < 16; i++) {
        video.dac[i * 3 + 0] = default_ega_dac[i][0];
        video.dac[i * 3 + 1] = default_ega_dac[i][1];
        video.dac[i * 3 + 2] = default_ega_dac[i][2];
    }
}

void video_set_dirty(void) { video.dirty = true; }
bool video_is_dirty(void) { return video.dirty; }
void video_clear_dirty(void) { video.dirty = false; }

/* ---- Port I/O ---- */

uint8_t video_portin(uint16_t port) {
    switch (port) {
        case 0x3C0:
            /* Attribute controller: read index */
            return video.attr_index;

        case 0x3C1:
            /* Attribute controller: read data */
            return video.attr[video.attr_index & 0x1F];

        case 0x3C4:
            return video.sc_index;

        case 0x3C5:
            return video.sc[video.sc_index];

        case 0x3C7:
            /* DAC state: 0=write mode, 3=read mode */
            return 0x03;

        case 0x3C8:
            return video.dac_index;

        case 0x3C9: {
            /* DAC data read */
            uint8_t val = video.dac[video.dac_read_idx * 3 + video.dac_read_step];
            video.dac_read_step++;
            if (video.dac_read_step >= 3) {
                video.dac_read_step = 0;
                video.dac_read_idx++;
            }
            return val;
        }

        case 0x3CC:
            /* Misc output register read */
            return video.misc_output;

        case 0x3CE:
            return video.gc_index;

        case 0x3CF:
            return video.gc[video.gc_index];

        case 0x3D4:
            return video.crtc_index;

        case 0x3D5:
            return video.crtc[video.crtc_index];

        case 0x3DA:
            /* Input Status 1: toggle retrace bits, reset attribute flipflop */
            video.attr_flipflop = false;
            s_retrace_toggle ^= 0x09;  /* Toggle bits 0 (hblank) and 3 (vblank) */
            return s_retrace_toggle;

        default:
            return 0xFF;
    }
}

void video_portout(uint16_t port, uint8_t val) {
    switch (port) {
        case 0x3C0:
            /* Attribute controller: alternates index/data writes */
            if (!video.attr_flipflop) {
                video.attr_index = val;
            } else {
                video.attr[video.attr_index & 0x1F] = val;
            }
            video.attr_flipflop = !video.attr_flipflop;
            break;

        case 0x3C2:
            /* Misc output register */
            video.misc_output = val;
            break;

        case 0x3C4:
            video.sc_index = val;
            break;

        case 0x3C5:
            video.sc[video.sc_index] = val;
            break;

        case 0x3C7:
            /* DAC read index */
            video.dac_read_idx = val;
            video.dac_read_step = 0;
            break;

        case 0x3C8:
            /* DAC write index */
            video.dac_index = val;
            video.dac_rgb_step = 0;
            break;

        case 0x3C9:
            /* DAC data write (R, G, B cycling) */
            video.dac[video.dac_index * 3 + video.dac_rgb_step] = val;
            video.dac_rgb_step++;
            if (video.dac_rgb_step >= 3) {
                video.dac_rgb_step = 0;
                video.dac_index++;
                video.dirty = true;
            }
            break;

        case 0x3CE:
            video.gc_index = val;
            break;

        case 0x3CF:
            video.gc[video.gc_index] = val;
            break;

        case 0x3D4:
            video.crtc_index = val;
            break;

        case 0x3D5:
            video.crtc[video.crtc_index] = val;
            /* Track cursor position from CRTC regs */
            if (video.crtc_index == 0x0E) {
                video.cursor_pos = (video.cursor_pos & 0x00FF) | ((uint16_t)val << 8);
            } else if (video.crtc_index == 0x0F) {
                video.cursor_pos = (video.cursor_pos & 0xFF00) | val;
            }
            break;

        default:
            break;
    }
}

/* ---- BIOS INT 10h ---- */

/* Scroll a rectangular region of text VRAM up by n lines */
static void scroll_up(uint8_t lines, uint8_t attr,
                       uint8_t row_top, uint8_t col_left,
                       uint8_t row_bot, uint8_t col_right) {
    if (lines == 0) {
        /* Clear entire window */
        for (int r = row_top; r <= row_bot; r++) {
            for (int c = col_left; c <= col_right; c++) {
                int off = (r * 80 + c) * 2;
                g_vram[off]     = 0x20;
                g_vram[off + 1] = attr;
            }
        }
    } else {
        for (int r = row_top; r <= row_bot; r++) {
            int src_row = r + lines;
            for (int c = col_left; c <= col_right; c++) {
                int dst = (r * 80 + c) * 2;
                if (src_row <= row_bot) {
                    int src = (src_row * 80 + c) * 2;
                    g_vram[dst]     = g_vram[src];
                    g_vram[dst + 1] = g_vram[src + 1];
                } else {
                    g_vram[dst]     = 0x20;
                    g_vram[dst + 1] = attr;
                }
            }
        }
    }
    video.dirty = true;
}

/* Scroll down by n lines */
static void scroll_down(uint8_t lines, uint8_t attr,
                          uint8_t row_top, uint8_t col_left,
                          uint8_t row_bot, uint8_t col_right) {
    if (lines == 0) {
        scroll_up(0, attr, row_top, col_left, row_bot, col_right);
        return;
    }
    for (int r = row_bot; r >= row_top; r--) {
        int src_row = r - lines;
        for (int c = col_left; c <= col_right; c++) {
            int dst = (r * 80 + c) * 2;
            if (src_row >= row_top) {
                int src = (src_row * 80 + c) * 2;
                g_vram[dst]     = g_vram[src];
                g_vram[dst + 1] = g_vram[src + 1];
            } else {
                g_vram[dst]     = 0x20;
                g_vram[dst + 1] = attr;
            }
        }
    }
    video.dirty = true;
}

void video_int10h(void) {
    uint8_t ah = regs.byteregs[regah];
    uint8_t al = regs.byteregs[regal];
    uint8_t bh = regs.byteregs[regbh];
    uint8_t bl = regs.byteregs[regbl];
    uint8_t ch_reg = regs.byteregs[regch];
    uint8_t cl_reg = regs.byteregs[regcl];
    uint8_t dh = regs.byteregs[regdh];
    uint8_t dl = regs.byteregs[regdl];
    uint16_t cx = regs.wordregs[regcx];

    switch (ah) {
        case 0x00: {
            /* Set video mode */
            uint8_t mode = al & 0x7F;
            bool no_clear = (al & 0x80) != 0;

            video.video_mode = mode;
            video.cursor_pos = 0;
            video.active_page = 0;

            if (!no_clear) {
                memset(g_vram, 0, VRAM_SIZE);
            }

            /* For text modes, fill with spaces + default attribute */
            if ((mode == 0x02 || mode == 0x03) && !no_clear) {
                for (int i = 0; i < 80 * 25; i++) {
                    g_vram[i * 2]     = 0x20;
                    g_vram[i * 2 + 1] = 0x07;
                }
            }

            /* Set mode in BIOS data area (BDA) */
            g_ram[0x449] = mode;
            g_ram[0x44A] = 80;  /* columns */
            g_ram[0x44B] = 0;
            g_ram[0x484] = 24;  /* rows - 1 */

            video.dirty = true;
            break;
        }

        case 0x01:
            /* Set cursor shape */
            video.cursor_start = ch_reg & 0x1F;
            video.cursor_end = cl_reg & 0x1F;
            break;

        case 0x02:
            /* Set cursor position */
            video.cursor_pos = (uint16_t)dh * 80 + dl;
            /* Store in BDA (page 0) */
            g_ram[0x450] = dl;
            g_ram[0x451] = dh;
            /* Update CRTC cursor position */
            video.crtc[0x0E] = (uint8_t)(video.cursor_pos >> 8);
            video.crtc[0x0F] = (uint8_t)(video.cursor_pos & 0xFF);
            break;

        case 0x03:
            /* Get cursor position */
            regs.byteregs[regdl] = (uint8_t)(video.cursor_pos % 80);
            regs.byteregs[regdh] = (uint8_t)(video.cursor_pos / 80);
            regs.byteregs[regch] = video.cursor_start;
            regs.byteregs[regcl] = video.cursor_end;
            break;

        case 0x05:
            /* Set active display page (stub) */
            video.active_page = al;
            break;

        case 0x06:
            /* Scroll up */
            scroll_up(al, bh, ch_reg, cl_reg, dh, dl);
            break;

        case 0x07:
            /* Scroll down */
            scroll_down(al, bh, ch_reg, cl_reg, dh, dl);
            break;

        case 0x08: {
            /* Read character and attribute at cursor */
            uint16_t off = video.cursor_pos * 2;
            regs.byteregs[regal] = g_vram[off];
            regs.byteregs[regah] = g_vram[off + 1];
            break;
        }

        case 0x09: {
            /* Write character and attribute at cursor, CX times */
            uint16_t off = video.cursor_pos * 2;
            for (uint16_t i = 0; i < cx; i++) {
                uint16_t pos = off + i * 2;
                if (pos + 1 < VRAM_SIZE) {
                    g_vram[pos]     = al;
                    g_vram[pos + 1] = bl;
                }
            }
            video.dirty = true;
            break;
        }

        case 0x0A: {
            /* Write character at cursor (keep current attribute), CX times */
            uint16_t off = video.cursor_pos * 2;
            for (uint16_t i = 0; i < cx; i++) {
                uint32_t pos = (uint32_t)off + (uint32_t)i * 2;
                if (pos < VRAM_SIZE) {
                    g_vram[pos] = al;
                }
            }
            video.dirty = true;
            break;
        }

        case 0x0E: {
            /* Teletype output */
            uint8_t col = (uint8_t)(video.cursor_pos % 80);
            uint8_t row = (uint8_t)(video.cursor_pos / 80);

            switch (al) {
                case 0x07:
                    /* BEL - ignore */
                    break;
                case 0x08:
                    /* Backspace */
                    if (col > 0) col--;
                    break;
                case 0x0A:
                    /* Line feed */
                    row++;
                    break;
                case 0x0D:
                    /* Carriage return */
                    col = 0;
                    break;
                default: {
                    /* Printable character */
                    uint16_t off = (row * 80 + col) * 2;
                    if (off + 1 < VRAM_SIZE) {
                        g_vram[off] = al;
                        g_vram[off + 1] = 0x07;  /* Default attribute */
                    }
                    col++;
                    if (col >= 80) {
                        col = 0;
                        row++;
                    }
                    break;
                }
            }

            /* Handle scrolling */
            if (row >= 25) {
                scroll_up(1, 0x07, 0, 0, 24, 79);
                row = 24;
            }

            video.cursor_pos = (uint16_t)row * 80 + col;
            /* Update BDA */
            g_ram[0x450] = col;
            g_ram[0x451] = row;
            /* Update CRTC */
            video.crtc[0x0E] = (uint8_t)(video.cursor_pos >> 8);
            video.crtc[0x0F] = (uint8_t)(video.cursor_pos & 0xFF);
            video.dirty = true;
            break;
        }

        case 0x0F:
            /* Get video mode */
            regs.byteregs[regal] = video.video_mode;
            regs.byteregs[regah] = 80;  /* columns */
            regs.byteregs[regbh] = video.active_page;
            break;

        case 0x10:
            /* DAC palette manipulation */
            switch (al) {
                case 0x00:
                    /* Set individual palette register */
                    video.attr[bl & 0x0F] = bh;
                    break;
                case 0x10:
                    /* Set individual DAC register */
                    video.dac[regs.wordregs[regbx] * 3 + 0] = regs.byteregs[regdh];
                    video.dac[regs.wordregs[regbx] * 3 + 1] = regs.byteregs[regch];
                    video.dac[regs.wordregs[regbx] * 3 + 2] = regs.byteregs[regcl];
                    video.dirty = true;
                    break;
                case 0x12: {
                    /* Set block of DAC registers */
                    uint16_t start = regs.wordregs[regbx];
                    uint16_t count = cx;
                    uint32_t src = segbase(segregs[reges]) + regs.wordregs[regdx];
                    for (uint16_t i = 0; i < count; i++) {
                        uint16_t idx = (start + i) & 0xFF;
                        video.dac[idx * 3 + 0] = read86(src++);
                        video.dac[idx * 3 + 1] = read86(src++);
                        video.dac[idx * 3 + 2] = read86(src++);
                    }
                    video.dirty = true;
                    break;
                }
                case 0x15: {
                    /* Read block of DAC registers */
                    uint16_t start = regs.wordregs[regbx];
                    uint16_t count = cx;
                    uint32_t dst = segbase(segregs[reges]) + regs.wordregs[regdx];
                    for (uint16_t i = 0; i < count; i++) {
                        uint16_t idx = (start + i) & 0xFF;
                        write86(dst++, video.dac[idx * 3 + 0]);
                        write86(dst++, video.dac[idx * 3 + 1]);
                        write86(dst++, video.dac[idx * 3 + 2]);
                    }
                    break;
                }
                case 0x17: {
                    /* Read individual DAC register */
                    uint16_t idx = regs.wordregs[regbx];
                    regs.byteregs[regdh] = video.dac[idx * 3 + 0];
                    regs.byteregs[regch] = video.dac[idx * 3 + 1];
                    regs.byteregs[regcl] = video.dac[idx * 3 + 2];
                    break;
                }
                default:
                    break;
            }
            break;

        case 0x11:
            /* Character generator (stub — return font info for mode 3) */
            if (al == 0x30) {
                /* Get font info */
                regs.byteregs[regdl] = 24;  /* rows - 1 */
                regs.wordregs[regcx] = 8;   /* bytes per character */
            }
            break;

        case 0x12:
            /* EGA info / alternate select */
            if (bl == 0x10) {
                regs.byteregs[regbh] = 0x00;  /* Color mode */
                regs.byteregs[regbl] = 0x03;  /* 256K EGA memory */
                regs.byteregs[regch] = 0x00;  /* Feature bits */
                regs.byteregs[regcl] = 0x00;  /* Switch settings */
            }
            break;

        case 0x1A:
            /* Get/set display combination code */
            if (al == 0x00) {
                regs.byteregs[regal] = 0x1A;  /* Function supported */
                regs.byteregs[regbl] = 0x08;  /* VGA with color analog display */
                regs.byteregs[regbh] = 0x00;  /* No secondary */
            }
            break;

        default:
            break;
    }
}
