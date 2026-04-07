#include "tcp.h"
#include "wifi.h"
#include "mongoose.h"
#include "umm_malloc.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>

// RP2350 XIP cache coherency — see comment in http.c
#define PSRAM_UNCACHED_OFFSET 0x04000000u
static inline uint8_t *rx_buf_uncached(const uint8_t *cached_ptr) {
  return (uint8_t *)((uintptr_t)cached_ptr + PSRAM_UNCACHED_OFFSET);
}

#define TCP_CB_CONNECT  (1 << 0)
#define TCP_CB_READ     (1 << 1)
#define TCP_CB_WRITE    (1 << 2)
#define TCP_CB_CLOSED   (1 << 3)
#define TCP_CB_FAILED   (1 << 4)

static tcp_conn_t s_conns[TCP_MAX_CONNECTIONS];

tcp_conn_t *tcp_get_conn(int idx) {
    if (idx < 0 || idx >= TCP_MAX_CONNECTIONS) return NULL;
    return &s_conns[idx];
}

void tcp_init(void) {
    memset(s_conns, 0, sizeof(s_conns));
}

tcp_conn_t *tcp_alloc(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!s_conns[i].in_use) {
            memset(&s_conns[i], 0, sizeof(tcp_conn_t));
            s_conns[i].rx_buf = umm_malloc(TCP_RECV_BUF_DEFAULT);
            if (!s_conns[i].rx_buf) return NULL;
            s_conns[i].rx_cap = TCP_RECV_BUF_DEFAULT;
            s_conns[i].spin_num = spin_lock_claim_unused(true);
            s_conns[i].spinlock = spin_lock_instance(s_conns[i].spin_num);
            s_conns[i].connect_timeout_ms = 15000;
            s_conns[i].read_timeout_ms = 0;  // disabled by default
            s_conns[i].in_use = true;
            return &s_conns[i];
        }
    }
    return NULL;
}

void tcp_free(tcp_conn_t *c) {
    if (!c) return;
    if (c->pcb) tcp_close(c);
    if (c->rx_buf) umm_free(c->rx_buf);
    int sn = c->spin_num;
    bool had_lock = (c->spinlock != NULL);
    memset(c, 0, sizeof(tcp_conn_t));
    if (had_lock) spin_lock_unclaim(sn);
}

bool tcp_connect(tcp_conn_t *c, const char *host, uint16_t port, bool use_ssl) {
    if (!c || !wifi_is_available()) return false;
    strncpy(c->host, host, sizeof(c->host) - 1);
    c->port = port;
    c->use_ssl = use_ssl;

    uint32_t save = spin_lock_blocking(c->spinlock);
    c->state = TCP_STATE_QUEUED;
    c->pending = 0;
    c->rx_head = c->rx_tail = c->rx_count = 0;
    spin_unlock(c->spinlock, save);

    c->deadline_connect = to_ms_since_boot(get_absolute_time()) + c->connect_timeout_ms;

    conn_req_t req = {.type = CONN_REQ_TCP_CONNECT, .conn = (http_conn_t*)c};
    return wifi_req_push(&req);
}

int tcp_write(tcp_conn_t *c, const void *buf, int len) {
    if (!c || c->state != TCP_STATE_CONNECTED || !c->pcb) return -1;
    
    void *copy = umm_malloc(len);
    if (!copy) return -1;
    memcpy(copy, buf, len);
    
    conn_req_t req = {
        .type = CONN_REQ_TCP_WRITE, 
        .conn = (http_conn_t*)c,
        .data = copy,
        .data_len = (uint32_t)len
    };
    
    if (!wifi_req_push(&req)) {
        umm_free(copy);
        return -1;
    }
    return len;
}

void tcp_close(tcp_conn_t *c) {
    if (!c || !c->pcb) return;
    conn_req_t req = {.type = CONN_REQ_TCP_CLOSE, .conn = (http_conn_t*)c};
    wifi_req_push(&req);
}

int tcp_read(tcp_conn_t *c, void *buf, int len) {
    if (!c || len == 0) return 0;

    uint32_t save = spin_lock_blocking(c->spinlock);
    if (c->rx_count == 0) {
        spin_unlock(c->spinlock, save);
        return 0;
    }
    uint32_t n = ((uint32_t)len < c->rx_count) ? (uint32_t)len : c->rx_count;

    // Read through uncached alias to see Core 1's writes to physical PSRAM
    const uint8_t *uc = rx_buf_uncached(c->rx_buf);
    uint32_t till_end = c->rx_cap - c->rx_tail;
    if (n <= till_end) {
        memcpy(buf, &uc[c->rx_tail], n);
        c->rx_tail += n;
        if (c->rx_tail == c->rx_cap) c->rx_tail = 0;
    } else {
        memcpy(buf, &uc[c->rx_tail], till_end);
        memcpy((uint8_t*)buf + till_end, uc, n - till_end);
        c->rx_tail = n - till_end;
    }
    c->rx_count -= n;
    spin_unlock(c->spinlock, save);
    return (int)n;
}

uint32_t tcp_bytes_available(tcp_conn_t *c) {
    if (!c) return 0;
    uint32_t save = spin_lock_blocking(c->spinlock);
    uint32_t n = c->rx_count;
    spin_unlock(c->spinlock, save);
    return n;
}

const char *tcp_get_error(tcp_conn_t *c) {
    return c ? c->err : NULL;
}

uint32_t tcp_take_pending(tcp_conn_t *c) {
    if (!c) return 0;
    uint32_t save = spin_lock_blocking(c->spinlock);
    uint32_t p = c->pending;
    c->pending = 0;
    spin_unlock(c->spinlock, save);
    return p;
}

void tcp_ev_fn(struct mg_connection *nc, int ev, void *ev_data) {
    tcp_conn_t *c = (tcp_conn_t *)nc->fn_data;
    if (!c) return;

    if (ev == MG_EV_CONNECT) {
        c->deadline_connect = 0;
        if (c->read_timeout_ms > 0)
            c->deadline_read = to_ms_since_boot(get_absolute_time()) + c->read_timeout_ms;
        uint32_t save = spin_lock_blocking(c->spinlock);
        c->state = TCP_STATE_CONNECTED;
        c->pending |= TCP_CB_CONNECT;
        spin_unlock(c->spinlock, save);
    } else if (ev == MG_EV_READ) {
        struct mg_iobuf *io = &nc->recv;

        uint32_t save = spin_lock_blocking(c->spinlock);
        uint32_t space = c->rx_cap - c->rx_count;
        uint32_t len = (uint32_t)io->len;
        if (len > space) len = space;

        if (len > 0) {
            // Write through uncached alias to bypass Core 1's XIP cache
            uint8_t *uc = rx_buf_uncached(c->rx_buf);
            uint32_t till_end = c->rx_cap - c->rx_head;
            if (len <= till_end) {
                memcpy(&uc[c->rx_head], io->buf, len);
                c->rx_head += len;
                if (c->rx_head == c->rx_cap) c->rx_head = 0;
            } else {
                memcpy(&uc[c->rx_head], io->buf, till_end);
                memcpy(uc, io->buf + till_end, len - till_end);
                c->rx_head = len - till_end;
            }
            c->rx_count += len;
            c->pending |= TCP_CB_READ;
        }
        spin_unlock(c->spinlock, save);

        if (len > 0) {
            mg_iobuf_del(io, 0, len);
            // Reset read deadline — data is actively arriving
            if (c->read_timeout_ms > 0)
                c->deadline_read = to_ms_since_boot(get_absolute_time()) + c->read_timeout_ms;
        }
    } else if (ev == MG_EV_ERROR) {
        snprintf(c->err, sizeof(c->err), "Mongoose error: %s", (char *)ev_data);
        uint32_t save = spin_lock_blocking(c->spinlock);
        c->state = TCP_STATE_FAILED;
        c->pending |= TCP_CB_FAILED;
        spin_unlock(c->spinlock, save);
    } else if (ev == MG_EV_CLOSE) {
        uint32_t save = spin_lock_blocking(c->spinlock);
        c->state = TCP_STATE_CLOSED;
        c->pending |= TCP_CB_CLOSED;
        c->pcb = NULL;
        spin_unlock(c->spinlock, save);
    }
}

void tcp_check_timeouts(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t *c = &s_conns[i];
        if (!c->in_use) continue;
        if ((c->state == TCP_STATE_QUEUED || c->state == TCP_STATE_CONNECTING) &&
            c->deadline_connect > 0 && now > c->deadline_connect) {
            snprintf(c->err, sizeof(c->err), "connect timeout (%ums)",
                     (unsigned)c->connect_timeout_ms);
            printf("[TCP] %s\n", c->err);
            uint32_t save = spin_lock_blocking(c->spinlock);
            c->state = TCP_STATE_FAILED;
            c->pending |= TCP_CB_FAILED;
            spin_unlock(c->spinlock, save);
            if (c->pcb) {
                ((struct mg_connection *)c->pcb)->is_closing = 1;
                c->pcb = NULL;
            }
            c->deadline_connect = 0;
        }
        // Read timeout: connected but no data arriving
        else if (c->state == TCP_STATE_CONNECTED &&
            c->deadline_read > 0 && now > c->deadline_read) {
            snprintf(c->err, sizeof(c->err), "read timeout (%ums)",
                     (unsigned)c->read_timeout_ms);
            printf("[TCP] %s\n", c->err);
            uint32_t save = spin_lock_blocking(c->spinlock);
            c->state = TCP_STATE_FAILED;
            c->pending |= TCP_CB_FAILED;
            spin_unlock(c->spinlock, save);
            if (c->pcb) {
                ((struct mg_connection *)c->pcb)->is_closing = 1;
                c->pcb = NULL;
            }
            c->deadline_read = 0;
        }
    }
}
