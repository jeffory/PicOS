#include "sim_socket.h"
#include "sim_socket_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#define MAX_CLIENTS 8
#define READ_BUF_SIZE 4096
#define UNIX_SOCK_PATH "./picos_control"
#define TCP_PORT 7878

typedef struct {
    int fd;
    int write_buf_used;
    char write_buf[READ_BUF_SIZE];
    char read_buf[READ_BUF_SIZE];
    int read_buf_used;
} client_t;

static client_t s_clients[MAX_CLIENTS];
static int s_unix_fd = -1;
static int s_tcp_fd = -1;
static int s_max_fd = -1;
static fd_set s_read_fds;
static pthread_mutex_t s_notify_mutex = PTHREAD_MUTEX_INITIALIZER;

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int add_client(int fd) {
    set_nonblocking(fd);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].fd <= 0) {
            s_clients[i].fd = fd;
            s_clients[i].write_buf_used = 0;
            s_clients[i].read_buf_used = 0;
            return 0;
        }
    }
    close(fd);
    return -1;
}

static void remove_client(int i) {
    if (s_clients[i].fd > 0) {
        close(s_clients[i].fd);
        s_clients[i].fd = -1;
        s_clients[i].write_buf_used = 0;
    }
}

static int flush_write_buf(client_t *c) {
    if (c->write_buf_used <= 0) return 0;
    ssize_t n = send(c->fd, c->write_buf, c->write_buf_used, MSG_NOSIGNAL);
    if (n > 0) {
        if ((size_t)n < (size_t)c->write_buf_used) {
            memmove(c->write_buf, c->write_buf + n, c->write_buf_used - n);
        }
        c->write_buf_used -= (int)n;
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return -1;
    }
    return 0;
}

static int queue_response(client_t *c, const char *json, size_t len) {
    if (c->fd <= 0) return -1;
    if (len == 0) len = strlen(json);
    while (len > 0) {
        size_t avail = READ_BUF_SIZE - 1 - c->write_buf_used;
        if (avail == 0) {
            if (flush_write_buf(c) < 0) return -1;
            avail = READ_BUF_SIZE - 1 - c->write_buf_used;
            if (avail == 0) return -1;
        }
        size_t chunk = len < avail ? len : avail;
        memcpy(c->write_buf + c->write_buf_used, json, chunk);
        c->write_buf_used += (int)chunk;
        json += chunk;
        len -= chunk;
    }
    return 0;
}

static void queue_notification(const char *json, size_t len) {
    pthread_mutex_lock(&s_notify_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].fd > 0) {
            queue_response(&s_clients[i], json, len);
        }
    }
    pthread_mutex_unlock(&s_notify_mutex);
}

static void process_requests(client_t *c) {
    while (c->read_buf_used > 0) {
        char *newline = memchr(c->read_buf, '\n', c->read_buf_used);
        if (!newline) {
            if (c->read_buf_used >= READ_BUF_SIZE) {
                c->read_buf_used = 0;
            }
            break;
        }
        *newline = '\0';
        size_t msg_len = newline - c->read_buf;

        char *resp = sim_handler_dispatch(c->read_buf, c->read_buf + msg_len);

        c->read_buf_used -= (int)(msg_len + 1);
        memmove(c->read_buf, newline + 1, c->read_buf_used);
        if (resp) {
            if (queue_response(c, resp, 0) < 0) {
                free(resp);
                return;
            }
            free(resp);
        }
    }
}

void sim_socket_init(void) {
    memset(s_clients, 0, sizeof(s_clients));
    for (int i = 0; i < MAX_CLIENTS; i++) s_clients[i].fd = -1;
    FD_ZERO(&s_read_fds);
    s_max_fd = -1;

    unlink(UNIX_SOCK_PATH);
    // ...

    s_unix_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s_unix_fd >= 0) {
        set_nonblocking(s_unix_fd);
        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, UNIX_SOCK_PATH, sizeof(addr.sun_path) - 1);
        if (bind(s_unix_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
            listen(s_unix_fd, 5) == 0) {
            printf("[Socket] UNIX domain server listening on %s\n", UNIX_SOCK_PATH);
        } else {
            close(s_unix_fd);
            s_unix_fd = -1;
        }
    }

    s_tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_tcp_fd >= 0) {
        int opt = 1;
        setsockopt(s_tcp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        set_nonblocking(s_tcp_fd);
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(TCP_PORT);
        if (bind(s_tcp_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
            listen(s_tcp_fd, 5) == 0) {
            printf("[Socket] TCP server listening on 0.0.0.0:%d\n", TCP_PORT);
        } else {
            close(s_tcp_fd);
            s_tcp_fd = -1;
        }
    }

    if (s_unix_fd < 0 && s_tcp_fd < 0) {
        printf("[Socket] WARNING: Failed to open any socket\n");
    }
}

void sim_socket_poll(void) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    int max_fd = -1;

    if (s_unix_fd >= 0) { FD_SET(s_unix_fd, &read_fds); if (s_unix_fd > max_fd) max_fd = s_unix_fd; }
    if (s_tcp_fd >= 0)  { FD_SET(s_tcp_fd, &read_fds);  if (s_tcp_fd > max_fd) max_fd = s_tcp_fd; }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].fd > 0) {
            FD_SET(s_clients[i].fd, &read_fds);
            if (s_clients[i].fd > max_fd) max_fd = s_clients[i].fd;
        }
    }

    struct timeval tv = {0, 0};
    int n = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
    if (n > 0) {
        printf("[Socket] select returned %d\n", n); fflush(stdout);
    }
    if (n <= 0) return;

    if (s_unix_fd >= 0 && FD_ISSET(s_unix_fd, &read_fds)) {
        int fd = accept(s_unix_fd, NULL, NULL);
        if (fd >= 0) { add_client(fd); }
    }
    if (s_tcp_fd >= 0 && FD_ISSET(s_tcp_fd, &read_fds)) {
        int fd = accept(s_tcp_fd, NULL, NULL);
        if (fd >= 0) { add_client(fd); }
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &s_clients[i];
        if (c->fd <= 0) continue;

        if (FD_ISSET(c->fd, &read_fds)) {
            ssize_t n = recv(c->fd, c->read_buf + c->read_buf_used,
                              READ_BUF_SIZE - c->read_buf_used - 1, 0);
            if (n <= 0) {
                remove_client(i);
                continue;
            }
            c->read_buf_used += (int)n;
            c->read_buf[c->read_buf_used] = '\0';
        }

        process_requests(c);

        if (c->fd > 0 && c->write_buf_used > 0) {
            if (flush_write_buf(c) < 0) {
                remove_client(i);
            }
        }
    }
}

void sim_socket_close(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].fd > 0) {
            close(s_clients[i].fd);
            s_clients[i].fd = -1;
        }
    }
    if (s_unix_fd >= 0) { close(s_unix_fd); s_unix_fd = -1; }
    if (s_tcp_fd >= 0)  { close(s_tcp_fd);  s_tcp_fd = -1; }
    unlink(UNIX_SOCK_PATH);
}

void sim_socket_notify(const char *method, const char *params_json) {
    static char buf[4096];
    int len = snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}\n",
                       method, params_json ? params_json : "{}");
    queue_notification(buf, (size_t)len);
}
