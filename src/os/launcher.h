#pragma once

// Runs the app launcher loop (never returns).
// Scans /apps/ on SD card, renders a scrollable menu, and launches selected apps.
void launcher_run(void);

// Re-scans the SD card for apps and resets the selection.
void launcher_refresh_apps(void);
