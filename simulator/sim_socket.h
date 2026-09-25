#pragma once
#include <stdbool.h>
#include <stddef.h>

// Initialize the RPC socket server.
// tcp_port: port to bind (0 = auto-assign, use sim_socket_get_port() after init)
// instance_id: if non-NULL, UNIX socket is ./picos_control_<id> instead of ./picos_control
// unix_path: if non-NULL, overrides the UNIX socket path; "none" disables the
//            UNIX socket (parallel test instances then never touch the cwd).
// The TCP listener binds 127.0.0.1 only: the RPCs read and write files.
void sim_socket_init(int tcp_port, const char *instance_id, const char *unix_path);
void sim_socket_poll(void);
void sim_socket_shutdown(void);
void sim_socket_close(void);
void sim_socket_notify(const char *method, const char *params_json);
// Queue an already-framed notification line to clients that subscribed to
// logs (subscribe {"logs":true}). Never blocks on a slow client.
void sim_socket_notify_log_subscribers(const char *json_line, size_t len);
// From an RPC handler: (un)subscribe the requesting client to logs.
void sim_socket_set_log_subscription(bool on);

// Returns the actual TCP port bound (useful when tcp_port=0 for auto-assign)
int sim_socket_get_port(void);
