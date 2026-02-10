#include "js_main.h"

js_conn_t *js_conn_create(const struct sockaddr *addr, socklen_t addr_len,
                             SSL_CTX *ssl_ctx, const char *hostname) {
    js_conn_t *c = calloc(1, sizeof(js_conn_t));
    if (!c) return NULL;

    c->socket.fd = socket(addr->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (c->socket.fd < 0) {
        free(c);
        return NULL;
    }

    /* TCP_NODELAY */
    int one = 1;
    setsockopt(c->socket.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    js_http_response_init(&c->response);

    int ret = connect(c->socket.fd, addr, addr_len);
    if (ret < 0 && errno != EINPROGRESS) {
        close(c->socket.fd);
        js_http_response_free(&c->response);
        free(c);
        return NULL;
    }

    c->state = CONN_CONNECTING;
    c->start_ns = js_now_ns();

    if (ssl_ctx) {
        c->ssl = js_tls_new(ssl_ctx, c->socket.fd, hostname);
    }

    return c;
}

void js_conn_free(js_conn_t *c) {
    if (!c) return;
    if (c->ssl) js_tls_free(c->ssl);
    if (c->socket.fd >= 0) close(c->socket.fd);
    js_buf_free(&c->in);
    js_http_response_free(&c->response);
    free(c);
}

int js_conn_set_request(js_conn_t *c, const char *data, size_t len) {
    c->req_data = data;
    c->req_len = len;
    c->req_sent = 0;
    return 0;
}

static int conn_do_write(js_conn_t *c) {
    while (c->req_sent < c->req_len) {
        ssize_t n;
        if (c->ssl) {
            n = js_tls_write(c->ssl, c->req_data + c->req_sent,
                              c->req_len - c->req_sent);
        } else {
            n = write(c->socket.fd, c->req_data + c->req_sent,
                      c->req_len - c->req_sent);
        }

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            c->state = CONN_ERROR;
            return -1;
        }
        if (n == 0) {
            c->state = CONN_ERROR;
            return -1;
        }
        c->req_sent += (size_t)n;
    }

    c->state = CONN_READING;
    return 0;
}

static void conn_try_handshake(js_conn_t *c) {
    int ret = js_tls_handshake(c->ssl);
    if (ret == 0) {
        c->state = CONN_WRITING;
        conn_do_write(c);
    } else if (ret < 0) {
        c->state = CONN_ERROR;
    }
    /* ret == 1: want more I/O, stay in TLS_HANDSHAKE */
}

void js_conn_process_write(js_conn_t *c) {
    switch (c->state) {
        case CONN_CONNECTING: {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(c->socket.fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err) {
                c->state = CONN_ERROR;
                return;
            }

            if (c->ssl) {
                c->state = CONN_TLS_HANDSHAKE;
                conn_try_handshake(c);
                return;
            }

            c->state = CONN_WRITING;
            conn_do_write(c);
            return;
        }
        case CONN_TLS_HANDSHAKE:
            conn_try_handshake(c);
            return;
        case CONN_WRITING:
            conn_do_write(c);
            return;
        default:
            return;
    }
}

bool js_conn_keepalive(const js_conn_t *c) {
    /* Check if server indicated keep-alive.
     * HTTP/1.1 defaults to keep-alive unless "Connection: close" is sent. */
    const char *conn_hdr = js_http_response_header(&c->response, "Connection");
    if (conn_hdr && strcasecmp(conn_hdr, "close") == 0)
        return false;
    return true;
}

void js_conn_reuse(js_conn_t *c) {
    /* Reuse existing connection: just reset parser and timing */
    js_buf_reset(&c->in);
    js_http_response_reset(&c->response);
    c->state = CONN_WRITING;
    c->req_sent = 0;
    c->start_ns = js_now_ns();
}

void js_conn_reset(js_conn_t *c, const struct sockaddr *addr,
                    socklen_t addr_len, SSL_CTX *ssl_ctx,
                    const char *hostname) {
    /* Close old connection */
    if (c->ssl) {
        js_tls_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->socket.fd >= 0) close(c->socket.fd);

    /* Reset buffers and parser */
    js_buf_reset(&c->in);
    js_http_response_reset(&c->response);

    /* New socket */
    c->socket.fd = socket(addr->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (c->socket.fd < 0) {
        c->state = CONN_ERROR;
        return;
    }

    int one = 1;
    setsockopt(c->socket.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    int ret = connect(c->socket.fd, addr, addr_len);
    if (ret < 0 && errno != EINPROGRESS) {
        close(c->socket.fd);
        c->socket.fd = -1;
        c->state = CONN_ERROR;
        return;
    }

    c->state = CONN_CONNECTING;
    c->req_sent = 0;
    c->start_ns = js_now_ns();

    if (ssl_ctx) {
        c->ssl = js_tls_new(ssl_ctx, c->socket.fd, hostname);
    }
}
