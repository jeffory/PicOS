#pragma once
#include <stddef.h>

// Initialize the RPC socket server.
// tcp_port: port to bind (0 = auto-assign, use sim_socket_get_port() after init)
// instance_id: if non-NULL, UNIX socket is ./picos_control_<id> instead of ./picos_control
void sim_socket_init(int tcp_port, const char *instance_id);
void sim_socket_poll(void);
void sim_socket_shutdown(void);
void sim_socket_close(void);
void sim_socket_notify(const char *method, const char *params_json);

// Returns the actual TCP port bound (useful when tcp_port=0 for auto-assign)
int sim_socket_get_port(void);
