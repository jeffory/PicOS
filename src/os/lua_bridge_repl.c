#include "lua_bridge_internal.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"
#include "../os/os.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TERM_COLS 53
#define TERM_ROWS 38
#define TERM_INPUT_ROW 37
#define SCROLLBACK_LINES 256
#define MAX_INPUT_LEN 256

typedef struct {
    char **lines;
    int line_count;
    int scroll_offset;
    int cursor_x;
    char input_buffer[MAX_INPUT_LEN];
    int input_len;
    bool echo;
} repl_state_t;

static repl_state_t *s_repl = NULL;

static void term_clear(void) {
    display_clear(COLOR_BLACK);
}

static void term_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg) {
    char s[2] = {c, '\0'};
    display_draw_text(x * 6, y * 8, s, fg, bg);
}

static void term_draw_line(int row, const char *line, int start_col, uint16_t fg, uint16_t bg) {
    int x = start_col;
    for (int i = 0; line[i] && x < TERM_COLS; i++, x++) {
        term_draw_char(x, row, line[i], fg, bg);
    }
    for (int x = start_col + strlen(line); x < TERM_COLS; x++) {
        term_draw_char(x, row, ' ', fg, bg);
    }
}

static void term_draw_input(void) {
    term_draw_line(TERM_INPUT_ROW, s_repl->input_buffer, 0, COLOR_WHITE, COLOR_DKGRAY);
    term_draw_char(s_repl->cursor_x, TERM_INPUT_ROW, ' ', COLOR_BLACK, COLOR_WHITE);
}

static void term_scroll_up(void) {
    if (s_repl->scroll_offset < s_repl->line_count - TERM_ROWS + 1) {
        s_repl->scroll_offset++;
    }
}

static void term_scroll_down(void) {
    if (s_repl->scroll_offset > 0) {
        s_repl->scroll_offset--;
    }
}

static void term_redraw(void) {
    display_clear(COLOR_BLACK);
    int row = 0;
    for (int i = s_repl->scroll_offset; i < s_repl->line_count && row < TERM_INPUT_ROW; i++) {
        term_draw_line(row, s_repl->lines[i], 0, COLOR_WHITE, COLOR_BLACK);
        row++;
    }
    for (; row < TERM_INPUT_ROW; row++) {
        term_draw_line(row, "", 0, COLOR_GRAY, COLOR_BLACK);
    }
    display_draw_line(0, TERM_INPUT_ROW * 8, 320, TERM_INPUT_ROW * 8, COLOR_DKGRAY);
    term_draw_input();
    display_flush();
}

static void term_add_line(const char *line) {
    if (s_repl->line_count >= SCROLLBACK_LINES) {
        free(s_repl->lines[0]);
        memmove(s_repl->lines, s_repl->lines + 1, (SCROLLBACK_LINES - 1) * sizeof(char*));
        s_repl->line_count--;
    }
    s_repl->lines[s_repl->line_count] = strdup(line);
    s_repl->line_count++;
    s_repl->scroll_offset = 0;
}

static void term_print(const char *text) {
    char buf[TERM_COLS + 1];
    int len = strlen(text);
    int pos = 0;
    
    for (int i = 0; i < len; i++) {
        if (text[i] == '\n' || pos >= TERM_COLS) {
            buf[pos] = '\0';
            term_add_line(buf);
            pos = 0;
            if (text[i] != '\n') {
                buf[pos++] = text[i];
            }
        } else {
            buf[pos++] = text[i];
        }
    }
    if (pos > 0) {
        buf[pos] = '\0';
        term_add_line(buf);
    }
}

static int l_repl_readline(lua_State *L) {
    kbd_poll();
    
    uint32_t pressed = kbd_get_buttons_pressed();
    
    if (pressed & BTN_ESC) {
        lua_pushnil(L);
        return 1;
    }
    
    if (pressed & BTN_UP) {
        term_scroll_up();
        term_redraw();
        kbd_clear_state();
        lua_pushnil(L);
        return 1;
    }
    
    if (pressed & BTN_DOWN) {
        term_scroll_down();
        term_redraw();
        kbd_clear_state();
        lua_pushnil(L);
        return 1;
    }
    
    if (pressed & BTN_ENTER) {
        lua_pushstring(L, s_repl->input_buffer);
        if (s_repl->echo) {
            term_add_line(s_repl->input_buffer);
        }
        s_repl->input_len = 0;
        s_repl->cursor_x = 0;
        term_redraw();
        return 1;
    }
    
    if (pressed & BTN_BACKSPACE) {
        if (s_repl->input_len > 0) {
            s_repl->input_len--;
            s_repl->cursor_x--;
            term_draw_input();
            display_flush();
        }
        kbd_clear_state();
        lua_pushnil(L);
        return 1;
    }
    
    char c = kbd_get_char();
    if (c && s_repl->input_len < MAX_INPUT_LEN - 1) {
        s_repl->input_buffer[s_repl->input_len++] = c;
        s_repl->input_buffer[s_repl->input_len] = '\0';
        s_repl->cursor_x++;
        term_draw_input();
        display_flush();
    }
    
    lua_pushnil(L);
    return 1;
}

static int l_repl_print(lua_State *L) {
    int n = lua_gettop(L);
    char buf[256];
    size_t pos = 0;
    
    for (int i = 1; i <= n; i++) {
        if (i > 1) {
            if (pos < sizeof(buf) - 1) buf[pos++] = '\t';
        }
        
        switch (lua_type(L, i)) {
            case LUA_TNIL:
                pos += snprintf(buf + pos, sizeof(buf) - pos, "nil");
                break;
            case LUA_TBOOLEAN:
                pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", lua_toboolean(L, i) ? "true" : "false");
                break;
            case LUA_TNUMBER:
                if (lua_isinteger(L, i)) {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "%lld", (long long)lua_tointeger(L, i));
                } else {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "%.17g", lua_tonumber(L, i));
                }
                break;
            case LUA_TSTRING: {
                size_t len;
                const char *s = lua_tolstring(L, i, &len);
                if (pos + len < sizeof(buf)) {
                    memcpy(buf + pos, s, len);
                    pos += len;
                }
                break;
            }
            case LUA_TTABLE: {
                pos += snprintf(buf + pos, sizeof(buf) - pos, "table: %p", lua_topointer(L, i));
                break;
            }
            case LUA_TFUNCTION: {
                pos += snprintf(buf + pos, sizeof(buf) - pos, "function: %p", lua_topointer(L, i));
                break;
            }
            case LUA_TUSERDATA: {
                pos += snprintf(buf + pos, sizeof(buf) - pos, "userdata: %p", lua_topointer(L, i));
                break;
            }
            default:
                pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", lua_typename(L, lua_type(L, i)));
                break;
        }
    }
    buf[pos] = '\0';
    term_print(buf);
    term_redraw();
    return 0;
}

static int l_repl_clear(lua_State *L) {
    (void)L;
    s_repl->line_count = 0;
    s_repl->scroll_offset = 0;
    term_clear();
    display_flush();
    return 0;
}

static int l_repl_echo(lua_State *L) {
    s_repl->echo = lua_toboolean(L, 1);
    return 0;
}

static const luaL_Reg l_repl_lib[] = {
    {"readline", l_repl_readline},
    {"print", l_repl_print},
    {"clear", l_repl_clear},
    {"echo", l_repl_echo},
    {NULL, NULL}
};

void lua_bridge_repl_init(lua_State *L) {
    s_repl = malloc(sizeof(repl_state_t));
    memset(s_repl, 0, sizeof(repl_state_t));
    
    s_repl->lines = calloc(SCROLLBACK_LINES, sizeof(char*));
    s_repl->echo = true;
    s_repl->cursor_x = 0;
    
    term_clear();
    display_flush();
    
    register_subtable(L, "repl", l_repl_lib);
}
