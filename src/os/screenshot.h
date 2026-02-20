#pragma once

// Save the current framebuffer as a BMP file to /screenshots/shotNNN.bmp.
// The file number increments automatically (001–999).
// Briefly displays a "Screenshot saved" overlay after writing.
void screenshot_save(void);
