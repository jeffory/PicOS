#include "lua_bridge_internal.h"
#include "../os/terminal.h"
#include "../os/terminal_parser.h"
#include "../os/terminal_render.h"
#include "umm_malloc.h"
#include "pico/time.h"

#define TERMINAL_MT "picocalc.terminal"

typedef struct {
    terminal_t* term;
    terminal_parser_t parser;
} lua_terminal_t;

static lua_terminal_t* check_terminal(lua_State* L, int idx) {
    return (lua_terminal_t*)luaL_checkudata(L, idx, TERMINAL_MT);
}

static int l_terminal_gc(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    if (t->term) {
        terminal_free(t->term);
        t->term = NULL;
    }
    return 0;
}

static int l_terminal_new(lua_State* L) {
    int cols = luaL_optinteger(L, 1, TERM_DEFAULT_COLS);
    int rows = luaL_optinteger(L, 2, TERM_DEFAULT_ROWS);
    int scrollback = luaL_optinteger(L, 3, TERM_DEFAULT_SCROLLBACK);

    terminal_t* term = terminal_new(cols, rows, scrollback);
    if (!term) {
        return luaL_error(L, "failed to create terminal");
    }

    lua_terminal_t* t = (lua_terminal_t*)lua_newuserdata(L, sizeof(lua_terminal_t));
    t->term = term;
    terminal_parser_init(&t->parser, term);

    luaL_setmetatable(L, TERMINAL_MT);
    return 1;
}

static int l_terminal_write(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    const char* str = luaL_checkstring(L, 2);
    
    terminal_parser_write(&t->parser, str);
    return 0;
}

static int l_terminal_clear(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    terminal_clear(t->term);
    return 0;
}

static int l_terminal_setCursor(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    terminal_setCursor(t->term, x, y);
    return 0;
}

static int l_terminal_getCursor(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int x, y;
    terminal_getCursor(t->term, &x, &y);
    lua_pushinteger(L, x);
    lua_pushinteger(L, y);
    return 2;
}

static int l_terminal_setColors(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    uint16_t fg = (uint16_t)luaL_checkinteger(L, 2);
    uint16_t bg = (uint16_t)luaL_checkinteger(L, 3);
    terminal_setColors(t->term, fg, bg);
    return 0;
}

static int l_terminal_getColors(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    uint16_t fg, bg;
    terminal_getColors(t->term, &fg, &bg);
    lua_pushinteger(L, fg);
    lua_pushinteger(L, bg);
    return 2;
}

static int l_terminal_scroll(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int lines = (int)luaL_checkinteger(L, 2);
    terminal_scroll(t->term, lines);
    return 0;
}

static int l_terminal_render(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    terminal_render(t->term);
    return 0;
}

static int l_terminal_renderDirty(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    terminal_renderDirty(t->term);
    return 0;
}

static int l_terminal_getCols(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    lua_pushinteger(L, terminal_getCols(t->term));
    return 1;
}

static int l_terminal_getRows(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    lua_pushinteger(L, terminal_getRows(t->term));
    return 1;
}

static int l_terminal_setCursorVisible(lua_State* L) {
    bool visible = lua_toboolean(L, 2);
    terminal_setCursorVisible(visible);
    return 0;
}

static int l_terminal_setCursorBlink(lua_State* L) {
    bool blink = lua_toboolean(L, 2);
    terminal_setCursorBlink(blink);
    return 0;
}

static int l_terminal_markAllDirty(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    terminal_markAllDirty(t->term);
    return 0;
}

static int l_terminal_isFullDirty(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    lua_pushboolean(L, terminal_isFullDirty(t->term));
    return 1;
}

static int l_terminal_getDirtyRange(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int first, last;
    terminal_getDirtyRange(t->term, &first, &last);
    lua_pushinteger(L, first);
    lua_pushinteger(L, last);
    return 2;
}

static int l_terminal_getScrollbackCount(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    lua_pushinteger(L, terminal_getScrollbackCount(t->term));
    return 1;
}

static int l_terminal_getScrollbackLine(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int line = (int)luaL_checkinteger(L, 2) - 1;
    
    int count = terminal_getScrollbackCount(t->term);
    if (line < 0 || line >= count) {
        return luaL_error(L, "scrollback line out of range");
    }
    
    uint16_t* cells = (uint16_t*)umm_malloc(t->term->cols * sizeof(uint16_t));
    if (!cells) return luaL_error(L, "out of memory");
    
    terminal_getScrollbackLine(t->term, line, cells);
    
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 0; i < t->term->cols; i++) {
        char c = cells[i] & 0xFF;
        if (c == 0) c = ' ';
        luaL_addchar(&b, c);
    }
    umm_free(cells);
    
    luaL_pushresult(&b);
    return 1;
}

static int l_terminal_getScrollbackOffset(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    lua_pushinteger(L, terminal_getScrollbackOffset(t->term));
    return 1;
}

static int l_terminal_setScrollbackOffset(lua_State* L) {
    lua_terminal_t* t = check_terminal(L, 1);
    int offset = (int)luaL_checkinteger(L, 2);
    terminal_setScrollbackOffset(t->term, offset);
    return 0;
}

static int l_terminal_waitForAnyKey(lua_State* L) {
    (void)L;
    while (true) {
        kbd_poll();
        uint32_t pressed = kbd_get_buttons();
        if (pressed != 0) {
            while (kbd_get_buttons() != 0) {
                kbd_poll();
                sleep_ms(10);
            }
            return 0;
        }
        sleep_ms(10);
    }
}

static const luaL_Reg terminal_methods[] = {
    {"write", l_terminal_write},
    {"clear", l_terminal_clear},
    {"setCursor", l_terminal_setCursor},
    {"getCursor", l_terminal_getCursor},
    {"setColors", l_terminal_setColors},
    {"getColors", l_terminal_getColors},
    {"scroll", l_terminal_scroll},
    {"render", l_terminal_render},
    {"renderDirty", l_terminal_renderDirty},
    {"getCols", l_terminal_getCols},
    {"getRows", l_terminal_getRows},
    {"setCursorVisible", l_terminal_setCursorVisible},
    {"setCursorBlink", l_terminal_setCursorBlink},
    {"markAllDirty", l_terminal_markAllDirty},
    {"isFullDirty", l_terminal_isFullDirty},
    {"getDirtyRange", l_terminal_getDirtyRange},
    {"getScrollbackCount", l_terminal_getScrollbackCount},
    {"getScrollbackLine", l_terminal_getScrollbackLine},
    {"getScrollbackOffset", l_terminal_getScrollbackOffset},
    {"setScrollbackOffset", l_terminal_setScrollbackOffset},
    {"waitForAnyKey", l_terminal_waitForAnyKey},
    {"__gc", l_terminal_gc},
    {NULL, NULL}
};

static const luaL_Reg terminal_funcs[] = {
    {"new", l_terminal_new},
    {NULL, NULL}
};

void lua_bridge_terminal_init(lua_State* L) {
    luaL_newmetatable(L, TERMINAL_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, terminal_methods, 0);
    lua_pop(L, 1);

    register_subtable(L, "terminal", terminal_funcs);
}
