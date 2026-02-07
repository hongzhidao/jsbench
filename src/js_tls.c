#include "js_main.h"

SSL_CTX *js_tls_ctx_create(void) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;

    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                          SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    /* Disable compression */
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);

    return ctx;
}

SSL *js_tls_new(SSL_CTX *ctx, int fd, const char *hostname) {
    SSL *ssl = SSL_new(ctx);
    if (!ssl) return NULL;

    SSL_set_fd(ssl, fd);
    SSL_set_connect_state(ssl);

    /* SNI */
    if (hostname)
        SSL_set_tlsext_host_name(ssl, hostname);

    return ssl;
}

int js_tls_handshake(SSL *ssl) {
    int ret = SSL_connect(ssl);
    if (ret == 1) return 0;  /* success */

    int err = SSL_get_error(ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        return 1;  /* try again */

    return -1;  /* error */
}

ssize_t js_tls_read(SSL *ssl, void *buf, size_t len) {
    int ret = SSL_read(ssl, buf, (int)len);
    if (ret > 0) return ret;

    int err = SSL_get_error(ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    if (err == SSL_ERROR_ZERO_RETURN) return 0;  /* peer closed */

    errno = EIO;
    return -1;
}

ssize_t js_tls_write(SSL *ssl, const void *buf, size_t len) {
    int ret = SSL_write(ssl, buf, (int)len);
    if (ret > 0) return ret;

    int err = SSL_get_error(ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }

    errno = EIO;
    return -1;
}

void js_tls_free(SSL *ssl) {
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
}
