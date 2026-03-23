#ifndef SIM_SOCKET_HANDLER_H
#define SIM_SOCKET_HANDLER_H

#include <stdbool.h>
#include <stdint.h>

void sim_handler_clear_pending_launch(void);
void sim_handler_request_exit(void);
const char *sim_handler_get_pending_launch(void);
void sim_handler_clear_pending_launch_state(void);
void sim_handler_check_launch(void);

void sim_log_append(const char *line);
char *sim_get_log_buffer(void);
int sim_get_log_buffer_count(void);

char *sim_handler_dispatch(const char *request, const char *end);

// Active terminal tracking (for terminal buffer dump in tests)
void sim_set_active_terminal(void *term);
void *sim_get_active_terminal(void);

#endif // SIM_SOCKET_HANDLER_H
