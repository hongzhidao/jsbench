#include "js_main.h"

static void conn_try_handshake(js_conn_t *c) {
    int ret = js_tls_handshake(c->ssl);
    if (ret == 0) {
        c->state = CONN_WRITING;
    } else if (ret < 0) {
        c->state = CONN_ERROR;
    }
    /* ret == 1: want more I/O, stay in TLS_HANDSHAKE */
}

static int conn_do_read(js_conn_t *c) {
    js_buf_t *in = &c->in;

    for (;;) {
        if (js_buf_ensure(in, in->len + JS_READ_BUF_SIZE) < 0) {
            c->state = CONN_ERROR;
            return -1;
        }

        ssize_t n;
        if (c->ssl) {
            n = js_tls_read(c->ssl, in->data + in->len, in->cap - in->len);
        } else {
            n = read(c->socket.fd, in->data + in->len, in->cap - in->len);
        }

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            c->state = CONN_ERROR;
            return -1;
        }
        if (n == 0) {
            return 1;  /* peer closed */
        }

        in->len += (size_t)n;
    }
}

int js_conn_read(js_conn_t *c) {
    switch (c->state) {
        case CONN_TLS_HANDSHAKE:
            conn_try_handshake(c);
            return 0;
        case CONN_READING:
            return conn_do_read(c);
        default:
            return 0;
    }
}
