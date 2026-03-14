#pragma once
#include <stdbool.h>

// Runs the app launcher loop (never returns).
// Scans /apps/ on SD card, renders a scrollable menu, and launches selected apps.
void launcher_run(void);

// Re-scans the SD card for apps and resets the selection.
void launcher_refresh_apps(void);

// List all installed apps to serial output (one per line)
void launcher_list_apps(void);

// Launch an app by name. Returns true if app was found and launched.
// Returns false if app not found.
bool launcher_launch_by_name(const char *name);

// Get the name of the currently running app (NULL if no app running)
const char* launcher_get_running_app_name(void);
