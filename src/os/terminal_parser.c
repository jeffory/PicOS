#include "terminal_parser.h"
#include "terminal.h"
#include <string.h>
#include <stdio.h>

#define RGB565(r, g, b) (((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))))

static const uint16_t TERM_PALETTE[] = {
    RGB565(0, 0, 0),         // 0: Black
    RGB565(170, 0, 0),       // 1: Red
    RGB565(0, 170, 0),       // 2: Green
    RGB565(170, 85, 0),     // 3: Yellow
    RGB565(0, 0, 170),      // 4: Blue
    RGB565(170, 0, 170),    // 5: Magenta
    RGB565(0, 170, 170),    // 6: Cyan
    RGB565(170, 170, 170),  // 7: White
    RGB565(85, 85, 85),     // 8: Bright Black (Gray)
    RGB565(255, 85, 85),    // 9: Bright Red
    RGB565(85, 255, 85),    // 10: Bright Green
    RGB565(255, 255, 85),   // 11: Bright Yellow
    RGB565(85, 85, 255),    // 12: Bright Blue
    RGB565(255, 85, 255),   // 13: Bright Magenta
    RGB565(85, 255, 255),   // 14: Bright Cyan
    RGB565(255, 255, 255),  // 15: Bright White
};

static uint16_t s_default_fg = RGB565(255, 255, 255);
static uint16_t s_default_bg = RGB565(0, 0, 0);

static uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return RGB565(r, g, b);
}

void terminal_parser_init(terminal_parser_t* parser, terminal_t* term) {
    memset(parser, 0, sizeof(terminal_parser_t));
    parser->terminal = term;
    parser->state = TERM_STATE_NORMAL;
    terminal_setColors(term, s_default_fg, s_default_bg);
}

void terminal_parser_reset(terminal_parser_t* parser) {
    parser->state = TERM_STATE_NORMAL;
    parser->param_count = 0;
    parser->current_param = 0;
    parser->has_intermediate = false;
    parser->bold = false;
    parser->italic = false;
    parser->underline = false;
    parser->blink = false;
    parser->inverse = false;
    parser->strikethrough = false;
    parser->dim = false;
    parser->hidden = false;
    if (parser->terminal) {
        terminal_setColors(parser->terminal, s_default_fg, s_default_bg);
    }
}

static void terminal_parser_process_csi(terminal_parser_t* parser) {
    terminal_t* term = parser->terminal;
    if (!term) return;

    char cmd = parser->intermediate;
    int p1 = parser->param_count > 0 ? parser->params[0] : 0;
    int p2 = parser->param_count > 1 ? parser->params[1] : 0;

    // Handle 24-bit color (38;2;R;G;B or 48;2;R;G;B)
    if (cmd == 'm' || cmd == 'h') {
        // Check for extended color sequences
        for (int i = 0; i < parser->param_count; i++) {
            int param = parser->params[i];
            
            // SGR - Select Graphic Rendition
            if (cmd == 'm') {
                switch (param) {
                    case 0: // Reset
                        terminal_parser_reset(parser);
                        break;
                    case 1: // Bold
                        parser->bold = true;
                        break;
                    case 3: // Italic
                        parser->italic = true;
                        break;
                    case 4: // Underline
                        parser->underline = true;
                        break;
                    case 5: // Blink
                    case 6:
                        parser->blink = true;
                        break;
                    case 7: // Inverse
                        parser->inverse = true;
                        break;
                    case 9: // Strikethrough
                        parser->strikethrough = true;
                        break;
                    case 2: // Dim
                        parser->dim = true;
                        break;
                    case 8: // Hidden
                        parser->hidden = true;
                        break;
                    case 22: // Normal (not bold/dim)
                        parser->bold = false;
                        parser->dim = false;
                        break;
                    case 23: // Not italic
                        parser->italic = false;
                        break;
                    case 24: // Not underline
                        parser->underline = false;
                        break;
                    case 25: // Not blink
                        parser->blink = false;
                        break;
                    case 27: // Not inverse
                        parser->inverse = false;
                        break;
                    case 29: // Not strikethrough
                        parser->strikethrough = false;
                        break;
                    case 30: case 31: case 32: case 33:
                    case 34: case 35: case 36: case 37: {
                        uint16_t cur_fg, cur_bg;
                        terminal_getColors(term, &cur_fg, &cur_bg);
                        terminal_setColors(term, TERM_PALETTE[param - 30], cur_bg);
                        break;
                    }
                    case 38: {
                        // Extended foreground color
                        uint16_t cur_fg, cur_bg;
                        terminal_getColors(term, &cur_fg, &cur_bg);
                        if (i + 4 < parser->param_count && parser->params[i + 1] == 2) {
                            // 24-bit color: 38;2;R;G;B
                            uint16_t fg = rgb888_to_rgb565(
                                parser->params[i + 2],
                                parser->params[i + 3],
                                parser->params[i + 4]
                            );
                            terminal_setColors(term, fg, cur_bg);
                            i += 4;
                        } else if (i + 2 < parser->param_count && parser->params[i + 1] == 5) {
                            // 256 color: 38;5;N
                            int color_idx = parser->params[i + 2];
                            if (color_idx < 16) {
                                terminal_setColors(term, TERM_PALETTE[color_idx], cur_bg);
                            }
                            i += 2;
                        }
                        break;
                    }
                    case 39: { // Default foreground
                        uint16_t cur_fg, cur_bg;
                        terminal_getColors(term, &cur_fg, &cur_bg);
                        terminal_setColors(term, s_default_fg, cur_bg);
                        break;
                    }
                    case 40: case 41: case 42: case 43:
                    case 44: case 45: case 46: case 47: {
                        uint16_t cur_fg, cur_bg;
                        terminal_getColors(term, &cur_fg, &cur_bg);
                        terminal_setColors(term, cur_fg, TERM_PALETTE[param - 40]);
                        break;
                    }
                    case 48: {
                        // Extended background color
                        uint16_t cur_fg, cur_bg;
                        terminal_getColors(term, &cur_fg, &cur_bg);
                        if (i + 4 < parser->param_count && parser->params[i + 1] == 2) {
                            // 24-bit color: 48;2;R;G;B
                            uint16_t bg = rgb888_to_rgb565(
                                parser->params[i + 2],
                                parser->params[i + 3],
                                parser->params[i + 4]
                            );
                            terminal_setColors(term, cur_fg, bg);
                            i += 4;
                        } else if (i + 2 < parser->param_count && parser->params[i + 1] == 5) {
                            // 256 color: 48;5;N
                            int color_idx = parser->params[i + 2];
                            if (color_idx < 16) {
                                terminal_setColors(term, cur_fg, TERM_PALETTE[color_idx]);
                            }
                            i += 2;
                        }
                        break;
                    }
                    case 49: { // Default background
                        uint16_t cur_fg, cur_bg;
                        terminal_getColors(term, &cur_fg, &cur_bg);
                        terminal_setColors(term, cur_fg, s_default_bg);
                        break;
                    }
                    case 90: case 91: case 92: case 93:
                    case 94: case 95: case 96: case 97: {
                        uint16_t cur_fg, cur_bg;
                        terminal_getColors(term, &cur_fg, &cur_bg);
                        terminal_setColors(term, TERM_PALETTE[param - 90 + 8], cur_bg);
                        break;
                    }
                    case 100: case 101: case 102: case 103:
                    case 104: case 105: case 106: case 107: {
                        uint16_t cur_fg, cur_bg;
                        terminal_getColors(term, &cur_fg, &cur_bg);
                        terminal_setColors(term, cur_fg, TERM_PALETTE[param - 100 + 8]);
                        break;
                    }
                }
            }
            
            // Inverse is handled by the renderer via TERM_ATTR_INVERSE
        }
        return;
    }

    switch (cmd) {
        case 'A': // CUU - Cursor Up
            if (p1 == 0) p1 = 1;
            terminal_setCursor(term, term->cursor_x, term->cursor_y - p1);
            break;

        case 'B': // CUD - Cursor Down
            if (p1 == 0) p1 = 1;
            terminal_setCursor(term, term->cursor_x, term->cursor_y + p1);
            break;

        case 'C': // CUF - Cursor Forward
            if (p1 == 0) p1 = 1;
            terminal_setCursor(term, term->cursor_x + p1, term->cursor_y);
            break;

        case 'D': // CUB - Cursor Back
            if (p1 == 0) p1 = 1;
            terminal_setCursor(term, term->cursor_x - p1, term->cursor_y);
            break;

        case 'E': // CNL - Cursor Next Line
            if (p1 == 0) p1 = 1;
            terminal_setCursor(term, 0, term->cursor_y + p1);
            break;

        case 'F': // CPL - Cursor Previous Line
            if (p1 == 0) p1 = 1;
            terminal_setCursor(term, 0, term->cursor_y - p1);
            break;

        case 'G': // CHA - Cursor Horizontal Absolute
            terminal_setCursor(term, p1 - 1, term->cursor_y);
            break;

        case 'H': // CUP - Cursor Position
        case 'f': // Also CUP
            if (p1 == 0) p1 = 1;
            if (p2 == 0) p2 = 1;
            terminal_setCursor(term, p2 - 1, p1 - 1);
            break;

        case 'J': // ED - Erase Display
            terminal_eraseDisplay(term, p1);
            break;

        case 'K': // EL - Erase Line
            terminal_eraseLine(term, p1);
            break;

        case 'L': // IL - Insert Lines
            if (p1 == 0) p1 = 1;
            for (int i = 0; i < p1; i++) {
                terminal_scrollDown(term);
            }
            break;

        case 'M': // DL - Delete Lines
            if (p1 == 0) p1 = 1;
            for (int i = 0; i < p1; i++) {
                terminal_scrollUp(term);
            }
            break;

        case 'S': // SU - Scroll Up
            if (p1 == 0) p1 = 1;
            terminal_scroll(term, p1);
            break;

        case 'T': // SD - Scroll Down
            if (p1 == 0) p1 = 1;
            terminal_scroll(term, -p1);
            break;

        case 's': // SCP - Save Cursor Position
            parser->saved_x = term->cursor_x;
            parser->saved_y = term->cursor_y;
            parser->saved_cursor_x = true;
            parser->saved_cursor_y = true;
            break;

        case 'u': // RCP - Restore Cursor Position
            if (parser->saved_cursor_x && parser->saved_cursor_y) {
                terminal_setCursor(term, parser->saved_x, parser->saved_y);
            }
            break;

        case 'n': // DSR - Device Status Report (ignore)
        case 'c': // DA - Device Attributes (ignore)
            break;
    }
}

void terminal_parser_parse(terminal_parser_t* parser, const char* data, int len) {
    if (!parser || !parser->terminal || !data) return;

    terminal_t* term = parser->terminal;

    for (int i = 0; i < len; i++) {
        uint8_t c = (uint8_t)data[i];

        switch (parser->state) {
            case TERM_STATE_NORMAL:
                if (c == 0x1B) { // ESC
                    parser->state = TERM_STATE_ESC;
                } else if (c == '\n' || c == '\r' || c == '\t' || c == '\b') {
                    terminal_putChar(term, (char)c);
                } else if (c >= 0x20) {
                    // Regular character - apply current attributes
                    uint8_t attr = 0;
                    if (parser->bold) attr |= TERM_ATTR_BOLD;
                    if (parser->italic) attr |= TERM_ATTR_ITALIC;
                    if (parser->underline) attr |= TERM_ATTR_UNDERLINE;
                    if (parser->blink) attr |= TERM_ATTR_BLINK;
                    if (parser->inverse) attr |= TERM_ATTR_INVERSE;
                    if (parser->strikethrough) attr |= TERM_ATTR_STRIKE;
                    if (parser->dim) attr |= TERM_ATTR_DIM;
                    if (parser->hidden) attr |= TERM_ATTR_HIDDEN;
                    
                    term->current_attr = attr;
                    terminal_putChar(term, (char)c);
                }
                break;

            case TERM_STATE_ESC:
                if (c == '[') {
                    parser->state = TERM_STATE_CSI;
                    parser->param_count = 0;
                    parser->current_param = 0;
                    parser->has_intermediate = false;
                    memset(parser->params, 0, sizeof(parser->params));
                } else if (c == ']') {
                    parser->state = TERM_STATE_OSC;
                } else if (c == '7') {
                    // Save cursor (ESC 7)
                    parser->saved_x = term->cursor_x;
                    parser->saved_y = term->cursor_y;
                    parser->saved_cursor_x = true;
                    parser->saved_cursor_y = true;
                    parser->state = TERM_STATE_NORMAL;
                } else if (c == '8') {
                    // Restore cursor (ESC 8)
                    if (parser->saved_cursor_x && parser->saved_cursor_y) {
                        terminal_setCursor(term, parser->saved_x, parser->saved_y);
                    }
                    parser->state = TERM_STATE_NORMAL;
                } else {
                    // Unknown escape sequence, ignore
                    parser->state = TERM_STATE_NORMAL;
                }
                break;

            case TERM_STATE_CSI:
                if (c >= '0' && c <= '9') {
                    // Digit - accumulate parameter
                    parser->current_param = parser->current_param * 10 + (c - '0');
                } else if (c == ';') {
                    // Parameter separator
                    if (parser->param_count < TERM_PARSER_MAX_PARAMS) {
                        parser->params[parser->param_count++] = parser->current_param;
                    }
                    parser->current_param = 0;
                } else if ((c >= '@' && c <= '~') && c != '[' && c != ']') {
                    // Final character - process command
                    if (parser->param_count < TERM_PARSER_MAX_PARAMS && parser->current_param > 0) {
                        parser->params[parser->param_count++] = parser->current_param;
                    } else if (parser->param_count < TERM_PARSER_MAX_PARAMS) {
                        parser->params[parser->param_count++] = 0;
                    }
                    parser->intermediate = c;
                    terminal_parser_process_csi(parser);
                    parser->state = TERM_STATE_NORMAL;
                } else if (c >= ' ' && c <= '/') {
                    // Intermediate character
                    parser->has_intermediate = true;
                    parser->intermediate = c;
                }
                break;

            case TERM_STATE_OSC:
                // OSC (Operating System Command) - ignore for now
                if (c == 0x07 || c == 0x1B) { // BEL or ESC
                    parser->state = TERM_STATE_NORMAL;
                }
                break;

            case TERM_STATE_ESC_DIGIT:
                // For private sequences, just go back to normal
                parser->state = TERM_STATE_NORMAL;
                break;
        }
    }
}

void terminal_parser_write(terminal_parser_t* parser, const char* str) {
    if (!parser || !str) return;
    terminal_parser_parse(parser, str, strlen(str));
}

uint16_t terminal_parser_getFG(terminal_parser_t* parser) {
    if (!parser) return s_default_fg;
    uint16_t fg, bg;
    terminal_getColors(parser->terminal, &fg, &bg);
    if (parser->inverse) return bg;
    return fg;
}

uint16_t terminal_parser_getBG(terminal_parser_t* parser) {
    if (!parser) return s_default_bg;
    uint16_t fg, bg;
    terminal_getColors(parser->terminal, &fg, &bg);
    if (parser->inverse) return fg;
    return bg;
}
