#pragma once
#include <stddef.h>

void sim_socket_init(void);
void sim_socket_poll(void);
void sim_socket_close(void);
void sim_socket_notify(const char *method, const char *params_json);
