#include "tcp.h"
#include "wifi.h"
#include "mongoose.h"
#include "umm_malloc.h"
#include <string.h>
#include <stdio.h>

#define TCP_CB_CONNECT  (1 << 0)
#define TCP_CB_READ     (1 << 1)
#define TCP_CB_WRITE    (1 << 2)
#define TCP_CB_CLOSED   (1 << 3)
#define TCP_CB_FAILED   (1 << 4)

static tcp_conn_t s_conns[TCP_MAX_CONNECTIONS];

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
    memset(c, 0, sizeof(tcp_conn_t));
}

bool tcp_connect(tcp_conn_t *c, const char *host, uint16_t port, bool use_ssl) {
    if (!c || !wifi_is_available()) return false;
    strncpy(c->host, host, sizeof(c->host) - 1);
    c->port = port;
    c->use_ssl = use_ssl;
    c->state = TCP_STATE_QUEUED;
    c->pending = 0;
    c->rx_head = c->rx_tail = c->rx_count = 0;
    
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
    if (!c || len == 0 || c->rx_count == 0) return 0;
    uint32_t n = (len < (int)c->rx_count) ? len : (uint32_t)len;
    if (n > c->rx_count) n = c->rx_count;

    uint32_t till_end = c->rx_cap - c->rx_tail;
    if (n <= till_end) {
        memcpy(buf, &c->rx_buf[c->rx_tail], n);
        c->rx_tail += n;
        if (c->rx_tail == c->rx_cap) c->rx_tail = 0;
    } else {
        memcpy(buf, &c->rx_buf[c->rx_tail], till_end);
        memcpy((uint8_t*)buf + till_end, c->rx_buf, n - till_end);
        c->rx_tail = n - till_end;
    }
    c->rx_count -= n;
    return (int)n;
}

uint32_t tcp_bytes_available(tcp_conn_t *c) {
    return c ? c->rx_count : 0;
}

const char *tcp_get_error(tcp_conn_t *c) {
    return c ? c->err : NULL;
}

uint32_t tcp_take_pending(tcp_conn_t *c) {
    if (!c) return 0;
    uint32_t p = c->pending;
    c->pending = 0;
    return p;
}

void tcp_ev_fn(struct mg_connection *nc, int ev, void *ev_data) {
    tcp_conn_t *c = (tcp_conn_t *)nc->fn_data;
    if (!c) return;

    if (ev == MG_EV_CONNECT) {
        c->state = TCP_STATE_CONNECTED;
        c->pending |= TCP_CB_CONNECT;
    } else if (ev == MG_EV_READ) {
        struct mg_iobuf *io = &nc->recv;
        uint32_t len = (uint32_t)io->len;
        uint32_t space = c->rx_cap - c->rx_count;
        if (len > space) len = space;
        
        if (len > 0) {
            uint32_t till_end = c->rx_cap - c->rx_head;
            if (len <= till_end) {
                memcpy(&c->rx_buf[c->rx_head], io->buf, len);
                c->rx_head += len;
                if (c->rx_head == c->rx_cap) c->rx_head = 0;
            } else {
                memcpy(&c->rx_buf[c->rx_head], io->buf, till_end);
                memcpy(c->rx_buf, io->buf + till_end, len - till_end);
                c->rx_head = len - till_end;
            }
            c->rx_count += len;
            mg_iobuf_del(io, 0, len);
            c->pending |= TCP_CB_READ;
        }
    } else if (ev == MG_EV_ERROR) {
        c->state = TCP_STATE_FAILED;
        snprintf(c->err, sizeof(c->err), "Mongoose error: %s", (char *)ev_data);
        c->pending |= TCP_CB_FAILED;
    } else if (ev == MG_EV_CLOSE) {
        c->state = TCP_STATE_CLOSED;
        c->pending |= TCP_CB_CLOSED;
        c->pcb = NULL;
    }
}
