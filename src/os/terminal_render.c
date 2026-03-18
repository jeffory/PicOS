#include "terminal_render.h"
#include "terminal.h"
#include "../drivers/display.h"
#include "../fonts/font_scientifica.h"
#include "umm_malloc.h"
#include "pico/time.h"
#include <string.h>

static bool s_cursor_visible = true;
static bool s_cursor_blink = true;
static uint32_t s_last_blink_time = 0;
static bool s_blink_state = false;

static const uint8_t* get_glyph(terminal_t* term, char c) {
    if (term->font == TERM_FONT_SCIENTIFICA_BOLD) {
        return font_scientifica_bold_glyph(c);
    }
    return font_scientifica_glyph(c);
}

static inline int get_font_width(terminal_t* term) {
    (void)term;
    return FONT_SCI_WIDTH;
}

static inline int get_font_height(terminal_t* term) {
    (void)term;
    return FONT_SCI_HEIGHT;
}

#define FONT_W get_font_width(term)
#define FONT_H get_font_height(term)

static inline uint16_t byte_swap(uint16_t val) {
    return (val >> 8) | ((val & 0xFF) << 8);
}

void terminal_render_init(void) {
    s_last_blink_time = 0;
    s_blink_state = false;
}

static void terminal_render_glyph(int x, int y, uint16_t fg, uint16_t bg,
                                   const uint8_t *glyph, int font_w, int font_h) {
    uint16_t* fb = display_get_back_buffer();
    uint16_t fg_be = byte_swap(fg);
    uint16_t bg_be = byte_swap(bg);

    for (int row = 0; row < font_h; row++) {
        uint8_t rowdata = glyph[row];
        int py = y + row;
        if (py < 0 || py >= 320) continue;
        for (int col = 0; col < font_w; col++) {
            int px = x + col;
            if (px >= 0 && px < 320) {
                fb[py * 320 + px] = (rowdata & (0x80 >> col)) ? fg_be : bg_be;
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
            const uint8_t *space = get_glyph(term, ' ');
            int fw = get_font_width(term);
            int fh = get_font_height(term);
            for (int col = 0; col < cols; col++) {
                terminal_render_glyph(col * fw, row * fh, 0xFFFF, 0x0000, space, fw, fh);
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

                const uint8_t *glyph = get_glyph(term, (char)(uint8_t)ch);
                terminal_render_glyph(col * FONT_W, row * FONT_H, fg, bg, glyph, FONT_W, FONT_H);
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

                    const uint8_t *glyph = get_glyph(term, (char)(uint8_t)ch);
                    terminal_render_glyph(col * FONT_W, row * FONT_H, fg, bg, glyph, FONT_W, FONT_H);
                }
            } else {
                const uint8_t *space = get_glyph(term, ' ');
                for (int col = 0; col < cols; col++) {
                    terminal_render_glyph(col * FONT_W, row * FONT_H, 0xFFFF, 0x0000, space, FONT_W, FONT_H);
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

            const uint8_t *glyph = get_glyph(term, (char)(uint8_t)ch);
            terminal_render_glyph(x, y, fg, bg, glyph, FONT_W, FONT_H);
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
            uint16_t cursor_fg = term->fg_colors[cursor_idx];
            uint16_t cursor_bg = term->bg_colors[cursor_idx];

            const uint8_t *glyph = get_glyph(term, (char)(uint8_t)ch);
            terminal_render_glyph(cx, cy, cursor_bg, cursor_fg, glyph, FONT_W, FONT_H);
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

            const uint8_t *glyph = get_glyph(term, (char)(uint8_t)ch);
            terminal_render_glyph(x, y, fg, bg, glyph, FONT_W, FONT_H);
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
            uint16_t cursor_fg = term->fg_colors[cursor_idx];
            uint16_t cursor_bg = term->bg_colors[cursor_idx];

            const uint8_t *glyph = get_glyph(term, (char)(uint8_t)ch);
            terminal_render_glyph(cx, cy, cursor_bg, cursor_fg, glyph, FONT_W, FONT_H);
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

        const uint8_t *glyph = get_glyph(term, (char)(uint8_t)ch);
        terminal_render_glyph(x, y, fg, bg, glyph, FONT_W, FONT_H);
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
