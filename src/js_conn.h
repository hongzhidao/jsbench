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

    /* Request data */
    const char      *req_data;
    size_t           req_len;
    size_t           req_sent;

    /* Read buffer */
    js_buf_t         in;

    /* Timing */
    uint64_t         start_ns;

    /* For round-robin in array mode */
    int              req_index;

    /* User data (for JS callbacks etc.) */
    void            *udata;
} js_conn_t;

int js_conn_read(js_conn_t *c);

#endif /* JS_CONN_H */
