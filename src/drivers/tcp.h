#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// =============================================================================
// TCP client over Mongoose/lwIP for PicOS
//
// Non-blocking, cross-core (Core 0 requests, Core 1 handles).
// =============================================================================

#define TCP_MAX_CONNECTIONS   4
#define TCP_RECV_BUF_DEFAULT  8192
#define TCP_ERR_MAX           128

typedef enum {
    TCP_STATE_IDLE = 0,
    TCP_STATE_QUEUED,
    TCP_STATE_CONNECTING,
    TCP_STATE_CONNECTED,
    TCP_STATE_CLOSED,
    TCP_STATE_FAILED,
} tcp_conn_state_t;

typedef struct {
    bool             in_use;
    tcp_conn_state_t state;
    
    char     host[128];
    uint16_t port;
    bool     use_ssl;
    
    char err[TCP_ERR_MAX];
    uint32_t pending;  // TCP_CB_* flags
    
    uint8_t *rx_buf;
    uint32_t rx_cap;
    uint32_t rx_head;
    uint32_t rx_tail;
    uint32_t rx_count;
    
    void *pcb;  // mongoose mg_connection
} tcp_conn_t;

void tcp_init(void);
tcp_conn_t *tcp_alloc(void);
void tcp_free(tcp_conn_t *c);
bool tcp_connect(tcp_conn_t *c, const char *host, uint16_t port, bool use_ssl);
int  tcp_write(tcp_conn_t *c, const void *buf, int len);
int  tcp_read(tcp_conn_t *c, void *buf, int len);
void tcp_close(tcp_conn_t *c);
uint32_t tcp_bytes_available(tcp_conn_t *c);
const char *tcp_get_error(tcp_conn_t *c);
uint32_t tcp_take_pending(tcp_conn_t *c);

// Mongoose event handler for TCP connections
struct mg_connection;
void tcp_ev_fn(struct mg_connection *nc, int ev, void *ev_data);
