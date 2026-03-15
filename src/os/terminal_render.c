#include "terminal_render.h"
#include "terminal.h"
#include "../drivers/display.h"
#include "../fonts/font_6x11.h"
#include "umm_malloc.h"
#include "pico/time.h"
#include <string.h>

static bool s_cursor_visible = true;
static bool s_cursor_blink = true;
static uint32_t s_last_blink_time = 0;
static bool s_blink_state = false;

#define FONT_W 6
#define FONT_H 11

static inline uint16_t byte_swap(uint16_t val) {
    return (val >> 8) | ((val & 0xFF) << 8);
}

void terminal_render_init(void) {
    s_last_blink_time = 0;
    s_blink_state = false;
}

static void terminal_render_glyph(int x, int y, uint16_t fg, uint16_t bg, const uint16_t* glyph) {
    uint16_t* fb = display_get_back_buffer();
    int fb_width = 320;

    for (int col = 0; col < FONT_W; col++) {
        uint16_t col_data = glyph[col];
        for (int row = 0; row < FONT_H; row++) {
            int px = x + col;
            int py = y + row;
            if (px >= 0 && px < fb_width && py >= 0 && py < 320) {
                uint16_t color = (col_data & (1 << row)) ? fg : bg;
                fb[py * fb_width + px] = byte_swap(color);
            }
        }
    }
}

void terminal_renderScrollback(terminal_t* term) {
    if (!term || term->scrollback_offset <= 0) return;

    int offset = term->scrollback_offset;
    int visible_rows = term->rows;
    int scrollback_count = term->scrollback_count;
    // Combined virtual document: scrollback lines then current screen lines
    // offset=0 is live (handled by caller), offset=N scrolls up N lines
    // Top visible virtual line:
    int top_virtual = scrollback_count + visible_rows - offset - visible_rows;
    // = scrollback_count - offset
    if (top_virtual < 0) top_virtual = 0;

    int cols = term->cols;
    uint16_t* line_cells = (uint16_t*)umm_malloc(cols * sizeof(uint16_t));
    uint16_t* line_fg = (uint16_t*)umm_malloc(cols * sizeof(uint16_t));
    uint16_t* line_bg = (uint16_t*)umm_malloc(cols * sizeof(uint16_t));
    if (!line_cells || !line_fg || !line_bg) {
        if (line_cells) umm_free(line_cells);
        if (line_fg) umm_free(line_fg);
        if (line_bg) umm_free(line_bg);
        return;
    }

    for (int row = 0; row < visible_rows; row++) {
        int vline = top_virtual + row;

        if (vline < 0) {
            // Before scrollback — blank
            for (int col = 0; col < cols; col++) {
                const uint16_t* glyph = font_6x11_glyph(' ');
                terminal_render_glyph(col * FONT_W, row * FONT_H, 0xFFFF, 0x0000, glyph);
            }
        } else if (vline < scrollback_count) {
            // Scrollback line — use stored colors
            terminal_getScrollbackLine(term, vline, line_cells);
            terminal_getScrollbackLineColors(term, vline, line_fg, line_bg);

            for (int col = 0; col < cols; col++) {
                uint16_t cell = line_cells[col];
                uint8_t ch = cell & 0xFF;
                uint8_t attr = (cell >> 8) & 0xFF;
                if (ch == 0) ch = ' ';

                uint16_t fg = line_fg[col];
                uint16_t bg = line_bg[col];
                if (attr & TERM_ATTR_INVERSE) {
                    uint16_t tmp = fg; fg = bg; bg = tmp;
                }

                const uint16_t* glyph = font_6x11_glyph((char)ch);
                if (!glyph) glyph = font_6x11_glyph(' ');
                terminal_render_glyph(col * FONT_W, row * FONT_H, fg, bg, glyph);
            }
        } else {
            // Current screen line
            int screen_row = vline - scrollback_count;
            if (screen_row < visible_rows) {
                int base = screen_row * cols;
                for (int col = 0; col < cols; col++) {
                    int idx = base + col;
                    uint16_t cell = term->cells[idx];
                    uint8_t ch = cell & 0xFF;
                    uint8_t attr = (cell >> 8) & 0xFF;

                    uint16_t fg = term->fg_colors[idx];
                    uint16_t bg = term->bg_colors[idx];
                    if (attr & TERM_ATTR_INVERSE) {
                        uint16_t tmp = fg; fg = bg; bg = tmp;
                    }

                    const uint16_t* glyph = font_6x11_glyph((char)ch);
                    if (!glyph) glyph = font_6x11_glyph(' ');
                    terminal_render_glyph(col * FONT_W, row * FONT_H, fg, bg, glyph);
                }
            } else {
                for (int col = 0; col < cols; col++) {
                    const uint16_t* glyph = font_6x11_glyph(' ');
                    terminal_render_glyph(col * FONT_W, row * FONT_H, 0xFFFF, 0x0000, glyph);
                }
            }
        }
    }

    umm_free(line_cells);
    umm_free(line_fg);
    umm_free(line_bg);

    terminal_clearFullDirty(term);
    for (int i = 0; i < term->rows; i++) {
        terminal_clearRowDirty(term, i);
    }
}

void terminal_render(terminal_t* term) {
    if (!term) return;

    if (term->scrollback_offset > 0) {
        terminal_renderScrollback(term);
        return;
    }

    for (int row = 0; row < term->rows; row++) {
        int y = row * FONT_H;
        if (y >= 320) break;

        for (int col = 0; col < term->cols; col++) {
            int x = col * FONT_W;
            if (x >= 320) break;

            int idx = row * term->cols + col;
            uint16_t cell = term->cells[idx];
            uint8_t ch = cell & 0xFF;
            uint8_t attr = (cell >> 8) & 0xFF;

            uint16_t fg = term->fg_colors[idx];
            uint16_t bg = term->bg_colors[idx];

            if (attr & TERM_ATTR_INVERSE) {
                uint16_t tmp = fg;
                fg = bg;
                bg = tmp;
            }

            const uint16_t* glyph = font_6x11_glyph((char)ch);
            if (!glyph) {
                glyph = font_6x11_glyph(' ');
            }

            terminal_render_glyph(x, y, fg, bg, glyph);
        }
    }

    if (s_cursor_visible && term->cursor_x >= 0 && term->cursor_x < term->cols &&
        term->cursor_y >= 0 && term->cursor_y < term->rows) {

        bool draw_cursor = true;
        if (s_cursor_blink) {
            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (now - s_last_blink_time > 500) {
                s_blink_state = !s_blink_state;
                s_last_blink_time = now;
            }
            draw_cursor = !s_blink_state;
        }

        if (draw_cursor) {
            int cx = term->cursor_x * FONT_W;
            int cy = term->cursor_y * FONT_H;

            int cursor_idx = term->cursor_y * term->cols + term->cursor_x;
            uint16_t cell = term->cells[cursor_idx];
            uint8_t ch = cell & 0xFF;
            uint16_t cursor_fg = term->fg_color;
            uint16_t cursor_bg = term->bg_color;

            const uint16_t* glyph = font_6x11_glyph((char)ch);
            if (!glyph) glyph = font_6x11_glyph(' ');
            terminal_render_glyph(cx, cy, cursor_bg, cursor_fg, glyph);
        }
    }

    terminal_clearFullDirty(term);
    for (int i = 0; i < term->rows; i++) {
        terminal_clearRowDirty(term, i);
    }
}

void terminal_renderDirty(terminal_t* term) {
    if (!term) return;

    if (term->scrollback_offset > 0) {
        terminal_renderScrollback(term);
        return;
    }

    int first_dirty, last_dirty;
    terminal_getDirtyRange(term, &first_dirty, &last_dirty);

    if (first_dirty < 0) return;

    if (terminal_isFullDirty(term)) {
        terminal_render(term);
        return;
    }

    for (int row = first_dirty; row <= last_dirty; row++) {
        if (row < 0 || row >= term->rows) continue;
        if (!terminal_isRowDirty(term, row)) continue;

        int y = row * FONT_H;

        for (int col = 0; col < term->cols; col++) {
            int x = col * FONT_W;

            int idx = row * term->cols + col;
            uint16_t cell = term->cells[idx];
            uint8_t ch = cell & 0xFF;
            uint8_t attr = (cell >> 8) & 0xFF;

            uint16_t fg = term->fg_colors[idx];
            uint16_t bg = term->bg_colors[idx];

            if (attr & TERM_ATTR_INVERSE) {
                uint16_t tmp = fg;
                fg = bg;
                bg = tmp;
            }

            const uint16_t* glyph = font_6x11_glyph((char)ch);
            if (!glyph) {
                glyph = font_6x11_glyph(' ');
            }

            terminal_render_glyph(x, y, fg, bg, glyph);
        }

        terminal_clearRowDirty(term, row);
    }

    if (s_cursor_visible && term->cursor_x >= 0 && term->cursor_x < term->cols &&
        term->cursor_y >= 0 && term->cursor_y < term->rows) {

        bool draw_cursor = true;
        if (s_cursor_blink) {
            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (now - s_last_blink_time > 500) {
                s_blink_state = !s_blink_state;
                s_last_blink_time = now;
            }
            draw_cursor = !s_blink_state;
        }

        if (draw_cursor) {
            int cx = term->cursor_x * FONT_W;
            int cy = term->cursor_y * FONT_H;

            int cursor_idx = term->cursor_y * term->cols + term->cursor_x;
            uint16_t cell = term->cells[cursor_idx];
            uint8_t ch = cell & 0xFF;
            uint16_t cursor_fg = term->fg_color;
            uint16_t cursor_bg = term->bg_color;

            const uint16_t* glyph = font_6x11_glyph((char)ch);
            if (!glyph) glyph = font_6x11_glyph(' ');
            terminal_render_glyph(cx, cy, cursor_bg, cursor_fg, glyph);
        }
    }
}

void terminal_renderRow(terminal_t* term, int row) {
    if (!term || row < 0 || row >= term->rows) return;

    int y = row * FONT_H;

    for (int col = 0; col < term->cols; col++) {
        int x = col * FONT_W;

        int idx = row * term->cols + col;
        uint16_t cell = term->cells[idx];
        uint8_t ch = cell & 0xFF;
        uint8_t attr = (cell >> 8) & 0xFF;

        uint16_t fg = term->fg_colors[idx];
        uint16_t bg = term->bg_colors[idx];

        if (attr & TERM_ATTR_INVERSE) {
            uint16_t tmp = fg;
            fg = bg;
            bg = tmp;
        }

        const uint16_t* glyph = font_6x11_glyph((char)ch);
        if (!glyph) {
            glyph = font_6x11_glyph(' ');
        }

        terminal_render_glyph(x, y, fg, bg, glyph);
    }

    terminal_clearRowDirty(term, row);
}

void terminal_setCursorVisible(bool visible) {
    s_cursor_visible = visible;
}

bool terminal_getCursorVisible(void) {
    return s_cursor_visible;
}

void terminal_setCursorBlink(bool blink) {
    s_cursor_blink = blink;
}

bool terminal_getCursorBlink(void) {
    return s_cursor_blink;
}
