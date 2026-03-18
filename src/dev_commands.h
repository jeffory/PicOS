#pragma once

#include <stdbool.h>

void dev_commands_init(void);

const char* dev_commands_get_device(void);

void dev_commands_poll(void);

bool dev_commands_process(void);

void dev_commands_send_screenshot(void);

bool dev_commands_wants_exit(void);

void dev_commands_clear_exit(void);

bool dev_commands_wants_usb(void);

void dev_commands_clear_usb(void);

bool dev_commands_wants_list(void);

void dev_commands_clear_list(void);

bool dev_commands_wants_reboot(void);

bool dev_commands_wants_reboot_flash(void);

const char* dev_commands_get_pending_launch(void);

void dev_commands_clear_pending_launch(void);
