#pragma once

#include <stdbool.h>

// =============================================================================
// File Browser Overlay
//
// A directory-navigation panel drawn on top of the current framebuffer,
// styled to match system_menu.c (same colour constants, ITEM_H, TITLE_H, etc).
//
// Usage:
//   char path[192];
//   if (file_browser_show("/data/myapp", path, sizeof(path))) {
//       // path now contains the selected file path
//   }
//
// Navigation:
//   Up/Down  — move selection
//   Enter    — enter directory or select file
//   Esc      — go up one directory, or cancel at root
// =============================================================================

// Show the file browser synchronously.
// start_path: initial directory to list (e.g. "/data/editor")
// out_path:   buffer to receive the full path of a selected file
// out_len:    size of out_path buffer
// Returns true if a file was selected, false if cancelled.
bool file_browser_show(const char *start_path, char *out_path, int out_len);
