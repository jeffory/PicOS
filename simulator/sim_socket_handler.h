#ifndef SIM_SOCKET_HANDLER_H
#define SIM_SOCKET_HANDLER_H

#include <stdbool.h>
#include <stdint.h>

// Launcher loop hook (main thread): runs a launch queued by the launch_app
// RPC and handles rescan_apps. Defined in sim_test_control.c.
bool sim_handler_check_launch(void);

char *sim_handler_dispatch(const char *request, const char *end);

// Active terminal tracking (for terminal buffer dump in tests)
void sim_set_active_terminal(void *term);
void *sim_get_active_terminal(void);

#endif // SIM_SOCKET_HANDLER_H
