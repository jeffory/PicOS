#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct terminal terminal_t;

typedef struct {
    terminal_t* (*create)(int cols, int rows, int scrollback_lines);
    void (*free)(terminal_t* term);
    void (*clear)(terminal_t* term);
    void (*write)(terminal_t* term, const char* str);
    void (*putChar)(terminal_t* term, char c);
    void (*setCursor)(terminal_t* term, int x, int y);
    void (*getCursor)(terminal_t* term, int* out_x, int* out_y);
    void (*setColors)(terminal_t* term, uint16_t fg, uint16_t bg);
    void (*getColors)(terminal_t* term, uint16_t* out_fg, uint16_t* out_bg);
    void (*scroll)(terminal_t* term, int lines);
    void (*render)(terminal_t* term);
    void (*renderDirty)(terminal_t* term);
    int (*getCols)(terminal_t* term);
    int (*getRows)(terminal_t* term);
    void (*setCursorVisible)(bool visible);
    void (*setCursorBlink)(bool blink);
    void (*markAllDirty)(terminal_t* term);
    bool (*isFullDirty)(terminal_t* term);
    void (*getDirtyRange)(terminal_t* term, int* out_first, int* out_last);
    int (*getScrollbackCount)(terminal_t* term);
    void (*setScrollbackOffset)(terminal_t* term, int offset);
    int (*getScrollbackOffset)(terminal_t* term);
    void (*getScrollbackLine)(terminal_t* term, int line, uint16_t* out_cells);
    void (*getScrollbackLineColors)(terminal_t* term, int line, uint16_t* out_fg, uint16_t* out_bg);
} picocalc_terminal_t;
