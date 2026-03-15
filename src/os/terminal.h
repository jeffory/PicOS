#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TERM_DEFAULT_COLS 53
#define TERM_DEFAULT_ROWS 29
#define TERM_DEFAULT_SCROLLBACK 1000

#define TERM_ATTR_BOLD      (1 << 0)
#define TERM_ATTR_ITALIC    (1 << 1)
#define TERM_ATTR_UNDERLINE (1 << 2)
#define TERM_ATTR_BLINK     (1 << 3)
#define TERM_ATTR_INVERSE   (1 << 4)
#define TERM_ATTR_STRIKE    (1 << 5)
#define TERM_ATTR_DIM       (1 << 6)
#define TERM_ATTR_HIDDEN    (1 << 7)

struct terminal {
    int cols;
    int rows;
    int cursor_x;
    int cursor_y;
    uint16_t fg_color;
    uint16_t bg_color;
    uint8_t current_attr;

    uint16_t* cells;
    uint16_t* prev_cells;
    uint16_t* fg_colors;    // per-cell foreground RGB565
    uint16_t* bg_colors;    // per-cell background RGB565

    int scrollback_lines;
    int scrollback_pos;
    int scrollback_count;
    int scrollback_offset;  // 0 = live view, >0 = lines scrolled up
    uint16_t* scrollback;
    uint16_t* scrollback_fg;   // per-cell foreground colors in scrollback
    uint16_t* scrollback_bg;   // per-cell background colors in scrollback

    bool full_dirty;
    uint8_t* row_dirty;
};

typedef struct terminal terminal_t;

terminal_t* terminal_new(int cols, int rows, int scrollback_lines);

void terminal_free(terminal_t* term);

void terminal_clear(terminal_t* term);

void terminal_clearRow(terminal_t* term, int row);

void terminal_putChar(terminal_t* term, char c);

void terminal_putString(terminal_t* term, const char* s);

void terminal_setCursor(terminal_t* term, int x, int y);

void terminal_getCursor(terminal_t* term, int* out_x, int* out_y);

void terminal_setColors(terminal_t* term, uint16_t fg, uint16_t bg);

void terminal_getColors(terminal_t* term, uint16_t* out_fg, uint16_t* out_bg);

void terminal_setAttribute(terminal_t* term, uint8_t attr);

void terminal_clearAttribute(terminal_t* term, uint8_t attr);

void terminal_scroll(terminal_t* term, int lines);

void terminal_scrollUp(terminal_t* term);

void terminal_scrollDown(terminal_t* term);

void terminal_eraseDisplay(terminal_t* term, int mode);

void terminal_eraseLine(terminal_t* term, int mode);

int terminal_getCols(terminal_t* term);

int terminal_getRows(terminal_t* term);

uint16_t terminal_getCell(terminal_t* term, int x, int y);

void terminal_setCell(terminal_t* term, int x, int y, uint16_t cell);

uint16_t terminal_getScrollback(terminal_t* term, int line);

int terminal_getScrollbackCount(terminal_t* term);

void terminal_getScrollbackLine(terminal_t* term, int line, uint16_t* out_cells);

void terminal_getScrollbackLineColors(terminal_t* term, int line, uint16_t* out_fg, uint16_t* out_bg);

void terminal_setScrollbackOffset(terminal_t* term, int offset);

int terminal_getScrollbackOffset(terminal_t* term);

void terminal_markAllDirty(terminal_t* term);

void terminal_markRowDirty(terminal_t* term, int row);

bool terminal_isRowDirty(terminal_t* term, int row);

void terminal_clearRowDirty(terminal_t* term, int row);

void terminal_getDirtyRange(terminal_t* term, int* out_first, int* out_last);

bool terminal_isFullDirty(terminal_t* term);

void terminal_clearFullDirty(terminal_t* term);
