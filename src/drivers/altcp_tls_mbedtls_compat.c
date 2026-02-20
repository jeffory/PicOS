/**
 * @file
 * PATCHED FOR MBEDTLS 3.x BY PICOS
 */

#include "lwip/opt.h"
#include "lwip/sys.h"

#if LWIP_ALTCP

#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/altcp_tcp.h"
#include "lwip/priv/altcp_priv.h"

#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509.h"
#include "mbedtls/ssl.h"
#include "mbedtls/error.h"
#include "mbedtls/debug.h"
#include "mbedtls/platform.h"
#include "mbedtls/version.h"
#include "mbedtls/net_sockets.h"

#include <string.h>
#include <stdlib.h>

#if MBEDTLS_VERSION_MAJOR >= 3
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#endif

typedef struct altcp_mbedtls_state_s {
  void *conf;
  struct altcp_pcb *inner_conn;
  mbedtls_ssl_context ssl_context;
  u8_t flags;
  u16_t rx_passed_unrecved;
  struct pbuf *rx;
  struct pbuf *rx_app;
  int bio_bytes_read;
  int bio_bytes_appl;
  int overhead_bytes_adjust;
} altcp_mbedtls_state_t;

#define ALTCP_MBEDTLS_FLAGS_HANDSHAKE_DONE    0x01
#define ALTCP_MBEDTLS_FLAGS_UPPER_CALLED      0x02
#define ALTCP_MBEDTLS_FLAGS_RX_CLOSE_QUEUED   0x04
#define ALTCP_MBEDTLS_FLAGS_RX_CLOSED         0x08

extern const struct altcp_functions altcp_mbedtls_functions;

struct altcp_tls_config {
  mbedtls_ssl_config conf;
};

struct altcp_tls_entropy_rng {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
};
static struct altcp_tls_entropy_rng *altcp_tls_entropy_rng_ptr;

static err_t altcp_mbedtls_lower_recv(void *arg, struct altcp_pcb *inner_conn, struct pbuf *p, err_t err);
static err_t altcp_mbedtls_setup(void *conf, struct altcp_pcb *conn, struct altcp_pcb *inner_conn);
static err_t altcp_mbedtls_lower_recv_process(struct altcp_pcb *conn, altcp_mbedtls_state_t *state);
static err_t altcp_mbedtls_handle_rx_appldata(struct altcp_pcb *conn, altcp_mbedtls_state_t *state);

static int altcp_mbedtls_bio_send(void *ctx, const unsigned char *dataptr, size_t size) {
  struct altcp_pcb *conn = (struct altcp_pcb *) ctx;
  altcp_mbedtls_state_t *state = (altcp_mbedtls_state_t *)conn->state;
  u16_t write_len = (u16_t)LWIP_MIN(size, 0xFFFF);
  err_t err = altcp_write(conn->inner_conn, (const void *)dataptr, write_len, TCP_WRITE_FLAG_COPY);
  if (err == ERR_OK) { state->overhead_bytes_adjust += (int)write_len; return (int)write_len; }
  return (err == ERR_MEM) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int altcp_mbedtls_bio_recv(void *ctx, unsigned char *buf, size_t len) {
  struct altcp_pcb *conn = (struct altcp_pcb *)ctx;
  altcp_mbedtls_state_t *state = (altcp_mbedtls_state_t *)conn->state;
  if (!state->rx) return MBEDTLS_ERR_SSL_WANT_READ;
  u16_t copy_len = (u16_t)LWIP_MIN(len, state->rx->len);
  u16_t ret = pbuf_copy_partial(state->rx, buf, copy_len, 0);
  pbuf_remove_header(state->rx, ret);
  if (state->rx->len == 0) {
    struct pbuf *p = state->rx;
    state->rx = p->next;
    p->next = NULL;
    pbuf_free(p);
  }
  state->bio_bytes_read += (int)ret;
  return (int)ret;
}

static err_t altcp_mbedtls_pass_rx_data(struct altcp_pcb *conn, altcp_mbedtls_state_t *state) {
  if (state->rx_app) {
    struct pbuf *buf = state->rx_app; state->rx_app = NULL;
    if (conn->recv) {
      u16_t tot_len = buf->tot_len;
      err_t err = conn->recv(conn->arg, conn, buf, ERR_OK);
      if (err != ERR_OK) { if (state->rx_app) pbuf_cat(buf, state->rx_app); state->rx_app = buf; return err; }
      state->rx_passed_unrecved += tot_len;
    } else { pbuf_free(buf); }
  } else if (state->flags & ALTCP_MBEDTLS_FLAGS_RX_CLOSE_QUEUED) {
    state->flags |= ALTCP_MBEDTLS_FLAGS_RX_CLOSED;
    if (conn->recv) return conn->recv(conn->arg, conn, NULL, ERR_OK);
  }
  return ERR_OK;
}

static err_t altcp_mbedtls_handle_rx_appldata(struct altcp_pcb *conn, altcp_mbedtls_state_t *state) {
  int ret; if (!(state->flags & ALTCP_MBEDTLS_FLAGS_HANDSHAKE_DONE)) return ERR_OK;
  do {
    struct pbuf *buf = pbuf_alloc(PBUF_RAW, PBUF_POOL_BUFSIZE, PBUF_POOL);
    if (!buf) return ERR_OK;
    ret = mbedtls_ssl_read(&state->ssl_context, (unsigned char *)buf->payload, PBUF_POOL_BUFSIZE);
    if (ret < 0) {
      pbuf_free(buf);
      if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        /* Acknowledge raw bytes consumed so TCP receive window stays open */
        if (state->bio_bytes_read) { altcp_recved(conn->inner_conn, (u16_t)state->bio_bytes_read); state->bio_bytes_read = 0; }
        return ERR_OK;
      }
      return ERR_CLSD;
    } else if (ret > 0) {
      pbuf_realloc(buf, (u16_t)ret); state->bio_bytes_appl += ret;
      if (!state->rx_app) state->rx_app = buf;
      else pbuf_cat(state->rx_app, buf);
    } else {
      pbuf_free(buf);
    }
  } while (ret > 0);
  /* Acknowledge raw bytes consumed by the SSL record layer */
  if (state->bio_bytes_read) { altcp_recved(conn->inner_conn, (u16_t)state->bio_bytes_read); state->bio_bytes_read = 0; }
  return altcp_mbedtls_pass_rx_data(conn, state);
}

static err_t altcp_mbedtls_lower_recv_process(struct altcp_pcb *conn, altcp_mbedtls_state_t *state) {
  if (!(state->flags & ALTCP_MBEDTLS_FLAGS_HANDSHAKE_DONE)) {
    int ret = mbedtls_ssl_handshake(&state->ssl_context);
    /* Flush any outgoing handshake data (e.g. ClientHello, ClientKeyExchange) */
    altcp_output(conn->inner_conn);
    /* Acknowledge consumed bytes so TCP receive window stays open during handshake */
    if (state->bio_bytes_read) {
      altcp_recved(conn->inner_conn, (u16_t)state->bio_bytes_read);
      state->bio_bytes_read = 0;
    }
    if (ret == 0) {
      state->flags |= ALTCP_MBEDTLS_FLAGS_HANDSHAKE_DONE;
      if (conn->connected) { err_t err = conn->connected(conn->arg, conn, ERR_OK); if (err != ERR_OK) return err; }
    } else if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      return ERR_CLSD;
    }
  }
  return altcp_mbedtls_handle_rx_appldata(conn, state);
}

static err_t altcp_mbedtls_lower_recv(void *arg, struct altcp_pcb *inner_conn, struct pbuf *p, err_t err) {
  struct altcp_pcb *conn = (struct altcp_pcb *)arg;
  if (!conn || !conn->state) { if (p) pbuf_free(p); return ERR_VAL; }
  altcp_mbedtls_state_t *state = (altcp_mbedtls_state_t *)conn->state;
  if (!p) { state->flags |= ALTCP_MBEDTLS_FLAGS_RX_CLOSE_QUEUED; return altcp_mbedtls_pass_rx_data(conn, state); }
  if (!state->rx) state->rx = p;
  else pbuf_cat(state->rx, p);
  return altcp_mbedtls_lower_recv_process(conn, state);
}

static err_t altcp_mbedtls_lower_sent(void *arg, struct altcp_pcb *inner_conn, u16_t len) {
  struct altcp_pcb *conn = (struct altcp_pcb *)arg;
  if (conn && conn->state && conn->sent) {
    altcp_mbedtls_state_t *state = (altcp_mbedtls_state_t *)conn->state;
    /* Only notify application once handshake is done; TLS overhead bytes are not app data */
    if (state->flags & ALTCP_MBEDTLS_FLAGS_HANDSHAKE_DONE) {
      return conn->sent(conn->arg, conn, len);
    }
  }
  return ERR_OK;
}

static void altcp_mbedtls_lower_err(void *arg, err_t err) {
  struct altcp_pcb *conn = (struct altcp_pcb *)arg;
  if (conn) {
    conn->inner_conn = NULL;
    if (conn->err) conn->err(conn->arg, err);
    altcp_free(conn);
  }
}

static err_t altcp_mbedtls_lower_connected(void *arg, struct altcp_pcb *inner_conn, err_t err) {
  struct altcp_pcb *conn = (struct altcp_pcb *)arg;
  if (conn && conn->state) {
    if (err != ERR_OK) { if (conn->connected) return conn->connected(conn->arg, conn, err); return err; }
    return altcp_mbedtls_lower_recv_process(conn, (altcp_mbedtls_state_t *)conn->state);
  }
  return ERR_VAL;
}

static err_t altcp_mbedtls_setup(void *conf, struct altcp_pcb *conn, struct altcp_pcb *inner_conn) {
  struct altcp_tls_config *config = (struct altcp_tls_config *)conf;
  altcp_mbedtls_state_t *state = (altcp_mbedtls_state_t *)malloc(sizeof(altcp_mbedtls_state_t));
  if (!state) return ERR_MEM;
  memset(state, 0, sizeof(altcp_mbedtls_state_t));
  state->conf = config;
  mbedtls_ssl_init(&state->ssl_context);
  if (mbedtls_ssl_setup(&state->ssl_context, &config->conf) != 0) { free(state); return ERR_MEM; }
  mbedtls_ssl_set_bio(&state->ssl_context, conn, altcp_mbedtls_bio_send, altcp_mbedtls_bio_recv, NULL);
  altcp_arg(inner_conn, conn);
  altcp_recv(inner_conn, altcp_mbedtls_lower_recv);
  altcp_sent(inner_conn, altcp_mbedtls_lower_sent);
  altcp_err(inner_conn, altcp_mbedtls_lower_err);
  conn->inner_conn = inner_conn;
  conn->fns = &altcp_mbedtls_functions;
  conn->state = state;
  return ERR_OK;
}

struct altcp_pcb *picos_altcp_tls_new(struct altcp_tls_config *config, u8_t ip_type) {
  struct altcp_pcb *inner_pcb = altcp_tcp_new_ip_type(ip_type);
  if (!inner_pcb) return NULL;
  struct altcp_pcb *ret = altcp_alloc();
  if (ret) {
    if (altcp_mbedtls_setup(config, ret, inner_pcb) != ERR_OK) { altcp_free(ret); altcp_close(inner_pcb); return NULL; }
  } else {
    altcp_close(inner_pcb);
  }
  return ret;
}

struct altcp_pcb *picos_altcp_tls_wrap(struct altcp_tls_config *config, struct altcp_pcb *inner_pcb) {
  struct altcp_pcb *ret = altcp_alloc();
  if (ret && altcp_mbedtls_setup(config, ret, inner_pcb) != ERR_OK) { altcp_free(ret); return NULL; }
  return ret;
}

static err_t altcp_mbedtls_connect(struct altcp_pcb *conn, const ip_addr_t *ipaddr, u16_t port, altcp_connected_fn connected) {
  if (!conn) return ERR_VAL;
  conn->connected = connected;
  return altcp_connect(conn->inner_conn, ipaddr, port, altcp_mbedtls_lower_connected);
}

static void altcp_mbedtls_dealloc(struct altcp_pcb *conn) {
  if (conn && conn->state) {
    altcp_mbedtls_state_t *state = (altcp_mbedtls_state_t *)conn->state;
    mbedtls_ssl_free(&state->ssl_context);
    if (state->rx) pbuf_free(state->rx);
    if (state->rx_app) pbuf_free(state->rx_app);
    free(state);
    conn->state = NULL;
  }
}

static err_t altcp_mbedtls_write(struct altcp_pcb *conn, const void *dataptr, u16_t len, u8_t apiflags) {
  if (!conn || !conn->state) return ERR_VAL;
  altcp_mbedtls_state_t *state = (altcp_mbedtls_state_t *)conn->state;
  if (!(state->flags & ALTCP_MBEDTLS_FLAGS_HANDSHAKE_DONE)) return ERR_VAL;
  int ret = mbedtls_ssl_write(&state->ssl_context, (const unsigned char *)dataptr, len);
  altcp_output(conn->inner_conn);
  if (ret >= 0) return ERR_OK;
  return (ret == MBEDTLS_ERR_SSL_WANT_WRITE) ? ERR_MEM : ERR_CLSD;
}

static u16_t altcp_mbedtls_mss(struct altcp_pcb *conn) { return conn ? altcp_mss(conn->inner_conn) : 0; }
static u16_t altcp_mbedtls_sndbuf(struct altcp_pcb *conn) { return conn ? altcp_sndbuf(conn->inner_conn) : 0; }

void *altcp_tls_context(struct altcp_pcb *conn) {
  if (conn && conn->state) {
    altcp_mbedtls_state_t *state = (altcp_mbedtls_state_t *)conn->state;
    return &state->ssl_context;
  }
  return NULL;
}

const struct altcp_functions altcp_mbedtls_functions = {
  NULL, NULL, NULL, altcp_mbedtls_connect, NULL, NULL, NULL, NULL,
  altcp_mbedtls_write, NULL, altcp_mbedtls_mss, altcp_mbedtls_sndbuf,
  NULL, NULL, NULL, NULL, NULL, altcp_mbedtls_dealloc, NULL, NULL, NULL,
  NULL, NULL, NULL
};

struct altcp_tls_config *altcp_tls_create_config_client(const u8_t *ca, size_t ca_len) {
  struct altcp_tls_config *conf = malloc(sizeof(struct altcp_tls_config));
  if (!conf) return NULL;
  memset(conf, 0, sizeof(struct altcp_tls_config));
  mbedtls_ssl_config_init(&conf->conf);
  if (!altcp_tls_entropy_rng_ptr) {
    altcp_tls_entropy_rng_ptr = malloc(sizeof(struct altcp_tls_entropy_rng));
    if (!altcp_tls_entropy_rng_ptr) return NULL;
    mbedtls_entropy_init(&altcp_tls_entropy_rng_ptr->entropy);
    mbedtls_ctr_drbg_init(&altcp_tls_entropy_rng_ptr->ctr_drbg);
    mbedtls_ctr_drbg_seed(&altcp_tls_entropy_rng_ptr->ctr_drbg, mbedtls_entropy_func, &altcp_tls_entropy_rng_ptr->entropy, NULL, 0);
  }
  mbedtls_ssl_config_defaults(&conf->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  mbedtls_ssl_conf_authmode(&conf->conf, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_rng(&conf->conf, mbedtls_ctr_drbg_random, &altcp_tls_entropy_rng_ptr->ctr_drbg);
  return conf;
}

void altcp_tls_free_config(struct altcp_tls_config *conf) {
  if (conf) { mbedtls_ssl_config_free(&conf->conf); free(conf); }
}

#endif
