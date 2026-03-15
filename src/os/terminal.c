#include "terminal.h"
#include "umm_malloc.h"
#include <string.h>
#include <stdio.h>

terminal_t* terminal_new(int cols, int rows, int scrollback_lines) {
    if (cols <= 0) cols = TERM_DEFAULT_COLS;
    if (rows <= 0) rows = TERM_DEFAULT_ROWS;
    if (scrollback_lines <= 0) scrollback_lines = TERM_DEFAULT_SCROLLBACK;

    terminal_t* term = (terminal_t*)umm_malloc(sizeof(terminal_t));
    if (!term) return NULL;

    term->cols = cols;
    term->rows = rows;
    term->scrollback_lines = scrollback_lines;
    term->cursor_x = 0;
    term->cursor_y = 0;
    term->fg_color = 0xFFFF;
    term->bg_color = 0x0000;
    term->current_attr = 0;
    term->full_dirty = true;
    term->scrollback_pos = 0;
    term->scrollback_count = 0;
    term->scrollback_offset = 0;

    int cell_count = cols * rows;
    term->cells = (uint16_t*)umm_malloc(cell_count * sizeof(uint16_t));
    term->prev_cells = (uint16_t*)umm_malloc(cell_count * sizeof(uint16_t));
    term->fg_colors = (uint16_t*)umm_malloc(cell_count * sizeof(uint16_t));
    term->bg_colors = (uint16_t*)umm_malloc(cell_count * sizeof(uint16_t));
    term->scrollback = (uint16_t*)umm_malloc(scrollback_lines * cols * sizeof(uint16_t));
    term->scrollback_fg = (uint16_t*)umm_malloc(scrollback_lines * cols * sizeof(uint16_t));
    term->scrollback_bg = (uint16_t*)umm_malloc(scrollback_lines * cols * sizeof(uint16_t));
    term->row_dirty = (uint8_t*)umm_malloc(rows * sizeof(uint8_t));

    if (!term->cells || !term->prev_cells || !term->fg_colors || !term->bg_colors ||
        !term->scrollback || !term->scrollback_fg || !term->scrollback_bg || !term->row_dirty) {
        terminal_free(term);
        return NULL;
    }

    memset(term->cells, 0, cell_count * sizeof(uint16_t));
    memset(term->prev_cells, 0xFF, cell_count * sizeof(uint16_t));
    for (int i = 0; i < cell_count; i++) {
        term->fg_colors[i] = 0xFFFF;
        term->bg_colors[i] = 0x0000;
    }
    memset(term->scrollback, 0, scrollback_lines * cols * sizeof(uint16_t));
    for (int i = 0; i < scrollback_lines * cols; i++) {
        term->scrollback_fg[i] = 0xFFFF;
        term->scrollback_bg[i] = 0x0000;
    }
    memset(term->row_dirty, 1, rows);

    return term;
}

void terminal_free(terminal_t* term) {
    if (!term) return;
    if (term->cells) umm_free(term->cells);
    if (term->prev_cells) umm_free(term->prev_cells);
    if (term->fg_colors) umm_free(term->fg_colors);
    if (term->bg_colors) umm_free(term->bg_colors);
    if (term->scrollback) umm_free(term->scrollback);
    if (term->scrollback_fg) umm_free(term->scrollback_fg);
    if (term->scrollback_bg) umm_free(term->scrollback_bg);
    if (term->row_dirty) umm_free(term->row_dirty);
    umm_free(term);
}

void terminal_clear(terminal_t* term) {
    if (!term) return;
    int cell_count = term->cols * term->rows;
    for (int i = 0; i < cell_count; i++) {
        term->cells[i] = 0x0020;
        term->fg_colors[i] = term->fg_color;
        term->bg_colors[i] = term->bg_color;
    }
    term->cursor_x = 0;
    term->cursor_y = 0;
    term->full_dirty = true;
    memset(term->row_dirty, 1, term->rows);
    term->scrollback_offset = 0;
}

void terminal_clearRow(terminal_t* term, int row) {
    if (!term || row < 0 || row >= term->rows) return;
    int base = row * term->cols;
    for (int x = 0; x < term->cols; x++) {
        term->cells[base + x] = 0x0020;
        term->fg_colors[base + x] = term->fg_color;
        term->bg_colors[base + x] = term->bg_color;
    }
    term->row_dirty[row] = 1;
}

static void terminal_addScrollback(terminal_t* term) {
    if (!term || term->scrollback_lines == 0) return;
    int line_size = term->cols;
    int offset = term->scrollback_pos * line_size;
    memcpy(&term->scrollback[offset], &term->cells[0], line_size * sizeof(uint16_t));
    memcpy(&term->scrollback_fg[offset], &term->fg_colors[0], line_size * sizeof(uint16_t));
    memcpy(&term->scrollback_bg[offset], &term->bg_colors[0], line_size * sizeof(uint16_t));
    term->scrollback_pos = (term->scrollback_pos + 1) % term->scrollback_lines;
    if (term->scrollback_count < term->scrollback_lines) {
        term->scrollback_count++;
    }
}

void terminal_putChar(terminal_t* term, char c) {
    if (!term) return;

    if (c == '\n') {
        term->cursor_x = 0;
        term->cursor_y++;
        if (term->cursor_y >= term->rows) {
            term->cursor_y = term->rows - 1;
            terminal_scrollUp(term);
        }
        return;
    }

    if (c == '\r') {
        term->cursor_x = 0;
        return;
    }

    if (c == '\t') {
        int tab_stop = 8;
        term->cursor_x = ((term->cursor_x / tab_stop) + 1) * tab_stop;
        if (term->cursor_x >= term->cols) {
            term->cursor_x = term->cols - 1;
        }
        return;
    }

    if (c == '\b') {
        if (term->cursor_x > 0) {
            term->cursor_x--;
        }
        return;
    }

    if (term->cursor_x >= term->cols) {
        term->cursor_x = 0;
        term->cursor_y++;
        if (term->cursor_y >= term->rows) {
            term->cursor_y = term->rows - 1;
            terminal_scrollUp(term);
        }
    }

    int idx = term->cursor_y * term->cols + term->cursor_x;
    uint16_t cell = (uint8_t)c | ((uint16_t)term->current_attr << 8);
    term->cells[idx] = cell;
    term->fg_colors[idx] = term->fg_color;
    term->bg_colors[idx] = term->bg_color;
    term->row_dirty[term->cursor_y] = 1;
    term->cursor_x++;
}

void terminal_putString(terminal_t* term, const char* s) {
    if (!term || !s) return;
    while (*s) {
        terminal_putChar(term, *s++);
    }
}

void terminal_setCursor(terminal_t* term, int x, int y) {
    if (!term) return;
    if (x < 0) x = 0;
    if (x >= term->cols) x = term->cols - 1;
    if (y < 0) y = 0;
    if (y >= term->rows) y = term->rows - 1;
    term->cursor_x = x;
    term->cursor_y = y;
}

void terminal_getCursor(terminal_t* term, int* out_x, int* out_y) {
    if (!term) return;
    if (out_x) *out_x = term->cursor_x;
    if (out_y) *out_y = term->cursor_y;
}

void terminal_setColors(terminal_t* term, uint16_t fg, uint16_t bg) {
    if (!term) return;
    term->fg_color = fg;
    term->bg_color = bg;
}

void terminal_getColors(terminal_t* term, uint16_t* out_fg, uint16_t* out_bg) {
    if (!term) return;
    if (out_fg) *out_fg = term->fg_color;
    if (out_bg) *out_bg = term->bg_color;
}

void terminal_setAttribute(terminal_t* term, uint8_t attr) {
    if (!term) return;
    term->current_attr |= attr;
}

void terminal_clearAttribute(terminal_t* term, uint8_t attr) {
    if (!term) return;
    term->current_attr &= ~attr;
}

void terminal_scroll(terminal_t* term, int lines) {
    if (!term || lines == 0) return;
    if (lines > 0) {
        for (int i = 0; i < lines; i++) {
            terminal_scrollUp(term);
        }
    } else {
        for (int i = 0; i < -lines; i++) {
            terminal_scrollDown(term);
        }
    }
}

void terminal_scrollUp(terminal_t* term) {
    if (!term) return;

    terminal_addScrollback(term);

    int line_size = term->cols * sizeof(uint16_t);
    memmove(&term->cells[0], &term->cells[term->cols], (term->rows - 1) * line_size);
    memmove(&term->fg_colors[0], &term->fg_colors[term->cols], (term->rows - 1) * line_size);
    memmove(&term->bg_colors[0], &term->bg_colors[term->cols], (term->rows - 1) * line_size);

    int last_row = (term->rows - 1) * term->cols;
    for (int x = 0; x < term->cols; x++) {
        term->cells[last_row + x] = 0x0020;
        term->fg_colors[last_row + x] = term->fg_color;
        term->bg_colors[last_row + x] = term->bg_color;
    }

    term->full_dirty = true;
    memset(term->row_dirty, 1, term->rows);
}

void terminal_scrollDown(terminal_t* term) {
    if (!term) return;

    int line_size = term->cols * sizeof(uint16_t);
    memmove(&term->cells[term->cols], &term->cells[0], (term->rows - 1) * line_size);
    memmove(&term->fg_colors[term->cols], &term->fg_colors[0], (term->rows - 1) * line_size);
    memmove(&term->bg_colors[term->cols], &term->bg_colors[0], (term->rows - 1) * line_size);

    for (int x = 0; x < term->cols; x++) {
        term->cells[x] = 0x0020;
        term->fg_colors[x] = term->fg_color;
        term->bg_colors[x] = term->bg_color;
    }

    term->full_dirty = true;
    memset(term->row_dirty, 1, term->rows);
}

static void terminal_erase_cell(terminal_t* term, int idx) {
    term->cells[idx] = 0x0020;
    term->fg_colors[idx] = term->fg_color;
    term->bg_colors[idx] = term->bg_color;
}

void terminal_eraseDisplay(terminal_t* term, int mode) {
    if (!term) return;

    if (mode == 0) {
        for (int y = term->cursor_y; y < term->rows; y++) {
            int start_x = (y == term->cursor_y) ? term->cursor_x : 0;
            for (int x = start_x; x < term->cols; x++) {
                terminal_erase_cell(term, y * term->cols + x);
            }
        }
    } else if (mode == 1) {
        for (int y = 0; y <= term->cursor_y; y++) {
            int end_x = (y == term->cursor_y) ? term->cursor_x + 1 : term->cols;
            for (int x = 0; x < end_x; x++) {
                terminal_erase_cell(term, y * term->cols + x);
            }
        }
    } else if (mode == 2) {
        int cell_count = term->cols * term->rows;
        for (int i = 0; i < cell_count; i++) {
            terminal_erase_cell(term, i);
        }
    }

    term->full_dirty = true;
    memset(term->row_dirty, 1, term->rows);
}

void terminal_eraseLine(terminal_t* term, int mode) {
    if (!term || term->cursor_y < 0 || term->cursor_y >= term->rows) return;

    int row = term->cursor_y;
    int base = row * term->cols;

    if (mode == 0) {
        for (int x = term->cursor_x; x < term->cols; x++) {
            terminal_erase_cell(term, base + x);
        }
    } else if (mode == 1) {
        for (int x = 0; x <= term->cursor_x; x++) {
            terminal_erase_cell(term, base + x);
        }
    } else if (mode == 2) {
        for (int x = 0; x < term->cols; x++) {
            terminal_erase_cell(term, base + x);
        }
    }

    term->row_dirty[row] = 1;
}

int terminal_getCols(terminal_t* term) {
    return term ? term->cols : 0;
}

int terminal_getRows(terminal_t* term) {
    return term ? term->rows : 0;
}

uint16_t terminal_getCell(terminal_t* term, int x, int y) {
    if (!term || x < 0 || x >= term->cols || y < 0 || y >= term->rows) return 0;
    return term->cells[y * term->cols + x];
}

void terminal_setCell(terminal_t* term, int x, int y, uint16_t cell) {
    if (!term || x < 0 || x >= term->cols || y < 0 || y >= term->rows) return;
    term->cells[y * term->cols + x] = cell;
    term->row_dirty[y] = 1;
}

uint16_t terminal_getScrollback(terminal_t* term, int line) {
    if (!term || line < 0 || line >= term->scrollback_count) return 0;
    int idx = (term->scrollback_pos - term->scrollback_count + line + term->scrollback_lines) % term->scrollback_lines;
    return term->scrollback[idx * term->cols];
}

int terminal_getScrollbackCount(terminal_t* term) {
    if (!term) return 0;
    return term->scrollback_count;
}

void terminal_getScrollbackLine(terminal_t* term, int line, uint16_t* out_cells) {
    if (!term || line < 0 || line >= term->scrollback_count || !out_cells) return;
    int idx = (term->scrollback_pos - term->scrollback_count + line + term->scrollback_lines) % term->scrollback_lines;
    memcpy(out_cells, &term->scrollback[idx * term->cols], term->cols * sizeof(uint16_t));
}

void terminal_getScrollbackLineColors(terminal_t* term, int line, uint16_t* out_fg, uint16_t* out_bg) {
    if (!term || line < 0 || line >= term->scrollback_count) return;
    int idx = (term->scrollback_pos - term->scrollback_count + line + term->scrollback_lines) % term->scrollback_lines;
    int offset = idx * term->cols;
    if (out_fg) memcpy(out_fg, &term->scrollback_fg[offset], term->cols * sizeof(uint16_t));
    if (out_bg) memcpy(out_bg, &term->scrollback_bg[offset], term->cols * sizeof(uint16_t));
}

void terminal_setScrollbackOffset(terminal_t* term, int offset) {
    if (!term) return;
    term->scrollback_offset = offset;
    term->full_dirty = true;
    memset(term->row_dirty, 1, term->rows);
}

int terminal_getScrollbackOffset(terminal_t* term) {
    return term ? term->scrollback_offset : -1;
}

void terminal_markAllDirty(terminal_t* term) {
    if (!term) return;
    term->full_dirty = true;
    memset(term->row_dirty, 1, term->rows);
}

void terminal_markRowDirty(terminal_t* term, int row) {
    if (!term || row < 0 || row >= term->rows) return;
    term->row_dirty[row] = 1;
}

bool terminal_isRowDirty(terminal_t* term, int row) {
    if (!term || row < 0 || row >= term->rows) return false;
    return term->row_dirty[row] != 0;
}

void terminal_clearRowDirty(terminal_t* term, int row) {
    if (!term || row < 0 || row >= term->rows) return;
    term->row_dirty[row] = 0;
}

void terminal_getDirtyRange(terminal_t* term, int* out_first, int* out_last) {
    if (!term) {
        if (out_first) *out_first = -1;
        if (out_last) *out_last = -1;
        return;
    }

    int first = -1;
    int last = -1;

    for (int i = 0; i < term->rows; i++) {
        if (term->row_dirty[i]) {
            if (first < 0) first = i;
            last = i;
        }
    }

    if (out_first) *out_first = first;
    if (out_last) *out_last = last;
}

bool terminal_isFullDirty(terminal_t* term) {
    return term ? term->full_dirty : false;
}

void terminal_clearFullDirty(terminal_t* term) {
    if (!term) return;
    term->full_dirty = false;
}
