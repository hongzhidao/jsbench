#include "jsb.h"

jsb_conn_t *jsb_conn_create(const struct sockaddr *addr, socklen_t addr_len,
                             SSL_CTX *ssl_ctx, const char *hostname) {
    jsb_conn_t *c = calloc(1, sizeof(jsb_conn_t));
    if (!c) return NULL;

    c->fd = socket(addr->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (c->fd < 0) {
        free(c);
        return NULL;
    }

    /* TCP_NODELAY */
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    jsb_http_response_init(&c->response);

    int ret = connect(c->fd, addr, addr_len);
    if (ret < 0 && errno != EINPROGRESS) {
        close(c->fd);
        jsb_http_response_free(&c->response);
        free(c);
        return NULL;
    }

    c->state = CONN_CONNECTING;
    c->start_ns = jsb_now_ns();

    if (ssl_ctx) {
        c->ssl = jsb_tls_new(ssl_ctx, c->fd, hostname);
    }

    return c;
}

void jsb_conn_free(jsb_conn_t *c) {
    if (!c) return;
    if (c->ssl) jsb_tls_free(c->ssl);
    if (c->fd >= 0) close(c->fd);
    jsb_http_response_free(&c->response);
    free(c);
}

int jsb_conn_set_request(jsb_conn_t *c, const char *data, size_t len) {
    c->req_data = data;
    c->req_len = len;
    c->req_sent = 0;
    return 0;
}

static int conn_do_write(jsb_conn_t *c) {
    while (c->req_sent < c->req_len) {
        ssize_t n;
        if (c->ssl) {
            n = jsb_tls_write(c->ssl, c->req_data + c->req_sent,
                              c->req_len - c->req_sent);
        } else {
            n = write(c->fd, c->req_data + c->req_sent,
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

static int conn_do_read(jsb_conn_t *c) {
    char buf[JSB_READ_BUF_SIZE];

    for (;;) {
        ssize_t n;
        if (c->ssl) {
            n = jsb_tls_read(c->ssl, buf, sizeof(buf));
        } else {
            n = read(c->fd, buf, sizeof(buf));
        }

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            c->state = CONN_ERROR;
            return -1;
        }
        if (n == 0) {
            /* Connection closed by peer */
            if (c->response.state != HTTP_PARSE_DONE) {
                /* If we were reading an identity body with no content-length, treat as done */
                if (c->response.state == HTTP_PARSE_BODY_IDENTITY ||
                    c->response.body_len > 0) {
                    c->state = CONN_DONE;
                    return 1;
                }
                c->state = CONN_ERROR;
                return -1;
            }
            c->state = CONN_DONE;
            return 1;
        }

        int ret = jsb_http_response_feed(&c->response, buf, (size_t)n);
        if (ret == 1) {
            c->state = CONN_DONE;
            return 1;
        }
        if (ret < 0) {
            c->state = CONN_ERROR;
            return -1;
        }
    }
}

int jsb_conn_handle_event(jsb_conn_t *c, uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) {
        c->state = CONN_ERROR;
        return -1;
    }

    switch (c->state) {
        case CONN_CONNECTING: {
            /* Check connect result */
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err) {
                c->state = CONN_ERROR;
                return -1;
            }

            if (c->ssl) {
                c->state = CONN_TLS_HANDSHAKE;
                /* Fall through to handshake */
            } else {
                c->state = CONN_WRITING;
                return conn_do_write(c);
            }
        }
        /* fallthrough */
        case CONN_TLS_HANDSHAKE: {
            int ret = jsb_tls_handshake(c->ssl);
            if (ret == 0) {
                c->state = CONN_WRITING;
                return conn_do_write(c);
            }
            if (ret == 1) return 0;  /* try again */
            c->state = CONN_ERROR;
            return -1;
        }

        case CONN_WRITING:
            return conn_do_write(c);

        case CONN_READING:
            return conn_do_read(c);

        default:
            return 0;
    }
}

bool jsb_conn_keepalive(const jsb_conn_t *c) {
    /* Check if server indicated keep-alive.
     * HTTP/1.1 defaults to keep-alive unless "Connection: close" is sent. */
    const char *conn_hdr = jsb_http_response_header(&c->response, "Connection");
    if (conn_hdr && strcasecmp(conn_hdr, "close") == 0)
        return false;
    return true;
}

void jsb_conn_reuse(jsb_conn_t *c) {
    /* Reuse existing connection: just reset parser and timing */
    jsb_http_response_reset(&c->response);
    c->state = CONN_WRITING;
    c->req_sent = 0;
    c->start_ns = jsb_now_ns();
}

void jsb_conn_reset(jsb_conn_t *c, const struct sockaddr *addr,
                    socklen_t addr_len, SSL_CTX *ssl_ctx,
                    const char *hostname) {
    /* Close old connection */
    if (c->ssl) {
        jsb_tls_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->fd >= 0) close(c->fd);

    /* Reset response parser */
    jsb_http_response_reset(&c->response);

    /* New socket */
    c->fd = socket(addr->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (c->fd < 0) {
        c->state = CONN_ERROR;
        return;
    }

    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    int ret = connect(c->fd, addr, addr_len);
    if (ret < 0 && errno != EINPROGRESS) {
        close(c->fd);
        c->fd = -1;
        c->state = CONN_ERROR;
        return;
    }

    c->state = CONN_CONNECTING;
    c->req_sent = 0;
    c->start_ns = jsb_now_ns();

    if (ssl_ctx) {
        c->ssl = jsb_tls_new(ssl_ctx, c->fd, hostname);
    }
}
