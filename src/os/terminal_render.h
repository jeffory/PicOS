#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "terminal.h"

void terminal_render_init(void);

void terminal_render(terminal_t* term);

void terminal_renderScrollback(terminal_t* term);

void terminal_renderDirty(terminal_t* term);

void terminal_renderRow(terminal_t* term, int row);

void terminal_setCursorVisible(bool visible);

bool terminal_getCursorVisible(void);

void terminal_setCursorBlink(bool blink);

bool terminal_getCursorBlink(void);
