#pragma once

#include <stdbool.h>

void dev_commands_init(void);

const char* dev_commands_get_device(void);

void dev_commands_poll(void);

bool dev_commands_process(void);

const char* dev_commands_get_pending_launch(void);

void dev_commands_clear_pending_launch(void);
