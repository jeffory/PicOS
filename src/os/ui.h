#pragma once

#include <stdbool.h>
#include <stdint.h>

// Draw standard OS header (titlebar) with battery/wifi/clock status indicators
void ui_draw_header(const char *title);

// Check if the header needs redrawing (e.g. clock or wifi status changed)
bool ui_needs_header_redraw(void);

// Draw standard OS footer with optional left and right alignment texts
void ui_draw_footer(const char *left_text, const char *right_text);

// Draw tab container with customizable keyboard shortcuts
// tabs: array of tab labels (NULL-terminated)
// count: number of tabs
// active_index: 0-based index of active tab (highlighted)
// y: vertical position
int ui_draw_tabs(const char **tabs, int count, int active_index, int y);

// Draw fullscreen splash screen with an optional status and subtext message
void ui_draw_splash(const char *status, const char *subtext);
