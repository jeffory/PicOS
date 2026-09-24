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
#include <time.h>
#include <stdbool.h>
#include <poll.h>

#define MAX_CLIENTS 8
// Per-client write buffer. Responses stream through it (queue_response waits
// for the client to drain it); notifications must fit whole or are dropped.
#define WRITE_BUF_SIZE (512 * 1024)
#define READ_BUF_INIT 4096
#define READ_BUF_MAX (256 * 1024)  // 256KB max for large base64 payloads
#define DEFAULT_UNIX_SOCK_PATH "./picos_control"
#define DEFAULT_TCP_PORT 7878

typedef struct {
    int fd;
    int write_buf_used;
    char write_buf[WRITE_BUF_SIZE];
    char *read_buf;
    int read_buf_used;
    int read_buf_cap;
    bool sub_logs;      // subscribed to `log` notifications
} client_t;

static client_t s_clients[MAX_CLIENTS];
static int s_unix_fd = -1;
static int s_tcp_fd = -1;
static int s_max_fd = -1;
static fd_set s_read_fds;
// Guards every client's fd and write buffer. The socket thread writes RPC
// responses and flushes; the main/Core-1 threads queue notifications. A
// response is copied into the buffer under one hold of this lock, so a
// notification can never land in the middle of it.
static pthread_mutex_t s_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
// Client whose request is being dispatched (socket thread only).
static client_t *s_dispatch_client = NULL;
// How long a response may wait for a slow client to drain its buffer.
#define RESPONSE_FLUSH_TIMEOUT_MS 5000
static int s_actual_tcp_port = 0;
static char s_unix_sock_path[256] = DEFAULT_UNIX_SOCK_PATH;

// Socket server thread — runs independently so MCP commands (screenshot, etc.)
// remain responsive even when the main thread is blocked by Unicorn emulation.
static pthread_t s_socket_thread;
static volatile bool s_socket_running = false;

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int add_client(int fd) {
    set_nonblocking(fd);
    pthread_mutex_lock(&s_clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].fd <= 0) {
            s_clients[i].fd = fd;
            s_clients[i].write_buf_used = 0;
            s_clients[i].read_buf_used = 0;
            s_clients[i].read_buf_cap = READ_BUF_INIT;
            s_clients[i].sub_logs = false;
            s_clients[i].read_buf = malloc(READ_BUF_INIT);
            if (!s_clients[i].read_buf) {
                close(fd);
                s_clients[i].fd = -1;
                pthread_mutex_unlock(&s_clients_mutex);
                return -1;
            }
            pthread_mutex_unlock(&s_clients_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&s_clients_mutex);
    close(fd);
    return -1;
}

static void remove_client(int i) {
    pthread_mutex_lock(&s_clients_mutex);
    if (s_clients[i].fd > 0) {
        close(s_clients[i].fd);
        s_clients[i].fd = -1;
        s_clients[i].write_buf_used = 0;
        s_clients[i].read_buf_used = 0;
        free(s_clients[i].read_buf);
        s_clients[i].read_buf = NULL;
        s_clients[i].read_buf_cap = 0;
        s_clients[i].sub_logs = false;
    }
    pthread_mutex_unlock(&s_clients_mutex);
}

// Caller holds s_clients_mutex.
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

// Caller holds s_clients_mutex. Copies the whole message or nothing.
// may_block (RPC responses, socket thread): stream through the buffer,
// waiting up to RESPONSE_FLUSH_TIMEOUT_MS for the client to drain it.
// !may_block (notifications, other threads): one flush attempt; if the
// message still does not fit it is dropped, never truncated.
static int queue_response(client_t *c, const char *json, size_t len, bool may_block) {
    if (c->fd <= 0) return -1;
    if (len == 0) len = strlen(json);
    if (!may_block) {
        size_t avail = WRITE_BUF_SIZE - 1 - c->write_buf_used;
        if (avail < len) {
            if (flush_write_buf(c) < 0) return -1;
            avail = WRITE_BUF_SIZE - 1 - c->write_buf_used;
            if (avail < len) return -1;
        }
        memcpy(c->write_buf + c->write_buf_used, json, len);
        c->write_buf_used += (int)len;
        return 0;
    }
    int waited_ms = 0;
    while (len > 0) {
        size_t avail = WRITE_BUF_SIZE - 1 - c->write_buf_used;
        if (avail == 0) {
            if (flush_write_buf(c) < 0) return -1;
            avail = WRITE_BUF_SIZE - 1 - c->write_buf_used;
            if (avail == 0) {
                if (waited_ms >= RESPONSE_FLUSH_TIMEOUT_MS) return -1;
                struct pollfd pfd = { .fd = c->fd, .events = POLLOUT };
                poll(&pfd, 1, 10);
                waited_ms += 10;
                continue;
            }
        }
        size_t chunk = len < avail ? len : avail;
        memcpy(c->write_buf + c->write_buf_used, json, chunk);
        c->write_buf_used += (int)chunk;
        json += chunk;
        len -= chunk;
    }
    return 0;
}

static void queue_notification(const char *json, size_t len, bool logs_only) {
    pthread_mutex_lock(&s_clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].fd > 0 && (!logs_only || s_clients[i].sub_logs)) {
            queue_response(&s_clients[i], json, len, false);
        }
    }
    pthread_mutex_unlock(&s_clients_mutex);
}

void sim_socket_notify_log_subscribers(const char *json_line, size_t len) {
    queue_notification(json_line, len, true);
}

void sim_socket_set_log_subscription(bool on) {
    // Only called from a handler, i.e. on the socket thread mid-dispatch.
    if (!s_dispatch_client) return;
    pthread_mutex_lock(&s_clients_mutex);
    s_dispatch_client->sub_logs = on;
    pthread_mutex_unlock(&s_clients_mutex);
}

static void process_requests(client_t *c) {
    while (c->read_buf_used > 0) {
        char *newline = memchr(c->read_buf, '\n', c->read_buf_used);
        if (!newline) {
            // Buffer full but no newline — try to grow
            if (c->read_buf_used >= c->read_buf_cap - 1) {
                if (c->read_buf_cap >= READ_BUF_MAX) {
                    // Hit max — discard
                    c->read_buf_used = 0;
                } else {
                    int new_cap = c->read_buf_cap * 2;
                    if (new_cap > READ_BUF_MAX) new_cap = READ_BUF_MAX;
                    char *new_buf = realloc(c->read_buf, new_cap);
                    if (new_buf) {
                        c->read_buf = new_buf;
                        c->read_buf_cap = new_cap;
                    } else {
                        c->read_buf_used = 0;
                    }
                }
            }
            break;
        }
        *newline = '\0';
        size_t msg_len = newline - c->read_buf;

        s_dispatch_client = c;
        char *resp = sim_handler_dispatch(c->read_buf, c->read_buf + msg_len);
        s_dispatch_client = NULL;

        c->read_buf_used -= (int)(msg_len + 1);
        memmove(c->read_buf, newline + 1, c->read_buf_used);
        if (resp) {
            pthread_mutex_lock(&s_clients_mutex);
            size_t rlen = strlen(resp);
            int rc = queue_response(c, resp, rlen, true);
            // Some error paths return an unterminated line; the protocol is
            // newline-delimited, so a client would otherwise wait forever.
            if (rc == 0 && rlen > 0 && resp[rlen - 1] != '\n')
                rc = queue_response(c, "\n", 1, true);
            pthread_mutex_unlock(&s_clients_mutex);
            free(resp);
            if (rc < 0) return;
        }
    }
}

static void *sim_socket_thread_func(void *arg);

void sim_socket_init(int tcp_port, const char *instance_id) {
    memset(s_clients, 0, sizeof(s_clients));
    for (int i = 0; i < MAX_CLIENTS; i++) s_clients[i].fd = -1;
    FD_ZERO(&s_read_fds);
    s_max_fd = -1;

    // Build UNIX socket path based on instance ID
    if (instance_id && instance_id[0]) {
        snprintf(s_unix_sock_path, sizeof(s_unix_sock_path),
                 "./picos_control_%s", instance_id);
    } else {
        strncpy(s_unix_sock_path, DEFAULT_UNIX_SOCK_PATH, sizeof(s_unix_sock_path) - 1);
    }

    unlink(s_unix_sock_path);

    s_unix_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s_unix_fd >= 0) {
        set_nonblocking(s_unix_fd);
        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, s_unix_sock_path, sizeof(addr.sun_path) - 1);
        if (bind(s_unix_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
            listen(s_unix_fd, 5) == 0) {
            printf("[Socket] UNIX domain server listening on %s\n", s_unix_sock_path);
        } else {
            close(s_unix_fd);
            s_unix_fd = -1;
        }
    }

    int bind_port = (tcp_port >= 0) ? tcp_port : DEFAULT_TCP_PORT;
    s_tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_tcp_fd >= 0) {
        int opt = 1;
        setsockopt(s_tcp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        set_nonblocking(s_tcp_fd);
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons((uint16_t)bind_port);
        if (bind(s_tcp_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
            listen(s_tcp_fd, 5) == 0) {
            // Query the actual port (needed when bind_port=0 for auto-assign)
            struct sockaddr_in bound = {0};
            socklen_t len = sizeof(bound);
            getsockname(s_tcp_fd, (struct sockaddr *)&bound, &len);
            s_actual_tcp_port = ntohs(bound.sin_port);
            printf("[Socket] TCP port: %d\n", s_actual_tcp_port);
        } else {
            close(s_tcp_fd);
            s_tcp_fd = -1;
        }
    }

    if (s_unix_fd < 0 && s_tcp_fd < 0) {
        printf("[Socket] WARNING: Failed to open any socket\n");
        return;
    }

    // Start the socket server thread so MCP commands remain responsive
    // even when the main thread is blocked (e.g., Unicorn emulation).
    s_socket_running = true;
    if (pthread_create(&s_socket_thread, NULL, sim_socket_thread_func, NULL) != 0) {
        fprintf(stderr, "[Socket] WARNING: Failed to create socket thread\n");
        s_socket_running = false;
    }
}

int sim_socket_get_port(void) {
    return s_actual_tcp_port;
}

void sim_socket_shutdown(void) {
    if (s_socket_running) {
        s_socket_running = false;
        pthread_join(s_socket_thread, NULL);
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
    // NOTE: no logging on this path. sim_socket_poll() runs every 10ms on the
    // socket thread, so anything printed here floods stdout — which deadlocked
    // the simulator whenever its stdout was an undrained pipe: the write blocks
    // inside fflush() while holding the stdio lock, and the main thread then
    // blocks in printf() waiting for it.
    int n = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
    // No early return when nothing is readable: notifications queued by
    // other threads must still be flushed. (They used to sit in the write
    // buffer until the client happened to send another request.)
    if (n < 0) FD_ZERO(&read_fds);

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
            // Grow buffer if full
            if (c->read_buf_used >= c->read_buf_cap - 1 && c->read_buf_cap < READ_BUF_MAX) {
                int new_cap = c->read_buf_cap * 2;
                if (new_cap > READ_BUF_MAX) new_cap = READ_BUF_MAX;
                char *new_buf = realloc(c->read_buf, new_cap);
                if (new_buf) {
                    c->read_buf = new_buf;
                    c->read_buf_cap = new_cap;
                }
            }
            ssize_t n = recv(c->fd, c->read_buf + c->read_buf_used,
                              c->read_buf_cap - c->read_buf_used - 1, 0);
            if (n <= 0) {
                remove_client(i);
                continue;
            }
            c->read_buf_used += (int)n;
            c->read_buf[c->read_buf_used] = '\0';
        }

        process_requests(c);

        pthread_mutex_lock(&s_clients_mutex);
        int flush_rc = (c->fd > 0 && c->write_buf_used > 0) ? flush_write_buf(c) : 0;
        pthread_mutex_unlock(&s_clients_mutex);
        if (flush_rc < 0) remove_client(i);
    }
}

static void *sim_socket_thread_func(void *arg) {
    (void)arg;
    while (s_socket_running) {
        sim_socket_poll();
        struct timespec ts = {0, 10000000};  // 10ms
        nanosleep(&ts, NULL);
    }
    return NULL;
}

void sim_socket_close(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].fd > 0) {
            close(s_clients[i].fd);
            s_clients[i].fd = -1;
        }
        free(s_clients[i].read_buf);
        s_clients[i].read_buf = NULL;
        s_clients[i].read_buf_cap = 0;
    }
    if (s_unix_fd >= 0) { close(s_unix_fd); s_unix_fd = -1; }
    if (s_tcp_fd >= 0)  { close(s_tcp_fd);  s_tcp_fd = -1; }
    unlink(s_unix_sock_path);
}

void sim_socket_notify(const char *method, const char *params_json) {
    // Heap-built: notifications come from several threads, and log/app
    // params can exceed any fixed buffer.
    const char *params = params_json ? params_json : "{}";
    size_t cap = strlen(method) + strlen(params) + 64;
    char *buf = malloc(cap);
    if (!buf) return;
    int len = snprintf(buf, cap, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}\n",
                       method, params);
    if (len > 0) queue_notification(buf, (size_t)len, false);
    free(buf);
}
