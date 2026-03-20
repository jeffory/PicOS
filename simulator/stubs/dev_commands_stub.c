// Dev commands stub for simulator
// Provides simulator-compatible implementations of dev command functions

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "../../src/os/screenshot.h"

// Command state
static bool s_cmd_exit = false;
static bool s_cmd_usb = false;
static bool s_cmd_reboot = false;
static bool s_cmd_reboot_flash = false;
static bool s_cmd_list = false;
static const char* s_pending_launch = NULL;

void dev_commands_init(void) {
    if (s_pending_launch) {
        free((void*)s_pending_launch);
        s_pending_launch = NULL;
    }
    s_cmd_exit = false;
    s_cmd_usb = false;
    s_cmd_reboot = false;
    s_cmd_reboot_flash = false;
    s_cmd_list = false;
    
    // Set stdin to non-blocking mode so getchar() doesn't block
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags != -1) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
}

const char* dev_commands_get_device(void) {
    return "/dev/ttyACM0";
}

// Forward declaration
void dev_commands_send_screenshot(void);

void dev_commands_poll(void) {
    // Check for commands from stdin (non-blocking)
    // In real hardware this comes from USB CDC serial
    static char s_cmd_buf[128];
    static int s_cmd_len = 0;
    
    int ch = fgetc(stdin);
    if (ch != EOF) {
        if (ch == '\n' || ch == '\r') {
            if (s_cmd_len > 0) {
                s_cmd_buf[s_cmd_len] = '\0';
                
                // Process command
                if (strcmp(s_cmd_buf, "exit") == 0) {
                    s_cmd_exit = true;
                } else if (strcmp(s_cmd_buf, "list") == 0) {
                    s_cmd_list = true;
                } else if (strcmp(s_cmd_buf, "screenshot") == 0) {
                    dev_commands_send_screenshot();
                } else if (strncmp(s_cmd_buf, "launch ", 7) == 0) {
                    s_pending_launch = strdup(s_cmd_buf + 7);
                    s_cmd_exit = true;  // Exit launcher to prepare for launch
                }
                
                s_cmd_len = 0;
            }
        } else if (s_cmd_len < sizeof(s_cmd_buf) - 1) {
            s_cmd_buf[s_cmd_len++] = (char)ch;
        }
    }
}

bool dev_commands_process(void) {
    // Process any pending commands
    // For now, just return false (no command processed)
    // Commands are set directly via dev_commands_set_exit() etc.
    return false;
}

void dev_commands_send_screenshot(void) {
    // Screenshot implementation for simulator - saves BMP to host filesystem
    extern void screenshot_save(void);
    screenshot_save();
}

// Exit command
bool dev_commands_wants_exit(void) {
    return s_cmd_exit;
}

void dev_commands_clear_exit(void) {
    s_cmd_exit = false;
}

void dev_commands_set_exit(void) {
    s_cmd_exit = true;
}

// USB command
bool dev_commands_wants_usb(void) {
    return s_cmd_usb;
}

void dev_commands_clear_usb(void) {
    s_cmd_usb = false;
}

// List command
bool dev_commands_wants_list(void) {
    return s_cmd_list;
}

void dev_commands_clear_list(void) {
    s_cmd_list = false;
}

// Reboot commands
bool dev_commands_wants_reboot(void) {
    return s_cmd_reboot;
}

bool dev_commands_wants_reboot_flash(void) {
    return s_cmd_reboot_flash;
}

// Launch command
const char* dev_commands_get_pending_launch(void) {
    return s_pending_launch;
}

void dev_commands_clear_pending_launch(void) {
    if (s_pending_launch) {
        free((void*)s_pending_launch);
        s_pending_launch = NULL;
    }
}

// Additional functions referenced by launcher
void dev_commands_send_keypress(const char* key) {
    (void)key;
    // Key injection handled via SDL in keyboard_stub.c
}
