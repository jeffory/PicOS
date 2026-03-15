#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "terminal.h"

#define TERM_PARSER_MAX_PARAMS 16

typedef enum {
    TERM_STATE_NORMAL,
    TERM_STATE_ESC,
    TERM_STATE_CSI,
    TERM_STATE_OSC,
    TERM_STATE_ESC_DIGIT,
} terminal_parser_state_t;

typedef struct terminal_parser {
    terminal_t* terminal;
    terminal_parser_state_t state;
    int params[TERM_PARSER_MAX_PARAMS];
    int param_count;
    int current_param;
    bool has_intermediate;
    uint8_t intermediate;
    bool saved_cursor_x;
    bool saved_cursor_y;
    int saved_x;
    int saved_y;
    bool bold;
    bool italic;
    bool underline;
    bool blink;
    bool inverse;
    bool strikethrough;
    bool dim;
    bool hidden;
} terminal_parser_t;

void terminal_parser_init(terminal_parser_t* parser, terminal_t* term);

void terminal_parser_parse(terminal_parser_t* parser, const char* data, int len);

void terminal_parser_write(terminal_parser_t* parser, const char* str);

void terminal_parser_reset(terminal_parser_t* parser);

uint16_t terminal_parser_getFG(terminal_parser_t* parser);

uint16_t terminal_parser_getBG(terminal_parser_t* parser);
