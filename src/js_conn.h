#ifndef JS_CONN_H
#define JS_CONN_H

/* ── Connection state ─────────────────────────────────────────────────── */

typedef enum {
    CONN_CONNECTING,
    CONN_TLS_HANDSHAKE,
    CONN_WRITING,
    CONN_READING,
    CONN_DONE,
    CONN_ERROR
} conn_state_t;

typedef struct js_conn {
    js_event_t       socket;  /* must be first: cast js_event_t* → js_conn_t* */
    conn_state_t     state;
    SSL             *ssl;

    /* I/O buffers */
    js_buf_t         out;
    js_buf_t         in;

    /* For round-robin in array mode */
    int              req_index;

    /* User data (for JS callbacks etc.) */
    void            *udata;
} js_conn_t;

js_conn_t *js_conn_create(const struct sockaddr *addr, socklen_t addr_len,
                             SSL_CTX *ssl_ctx, const char *hostname);
void        js_conn_free(js_conn_t *c);
int         js_conn_set_output(js_conn_t *c, const char *data, size_t len);
void        js_conn_reset(js_conn_t *c, const struct sockaddr *addr,
                           socklen_t addr_len, SSL_CTX *ssl_ctx,
                           const char *hostname);
void        js_conn_reuse(js_conn_t *c);
void        js_conn_write(js_conn_t *c);
int         js_conn_read(js_conn_t *c);

#endif /* JS_CONN_H */
