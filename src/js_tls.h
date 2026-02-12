#ifndef JS_TLS_H
#define JS_TLS_H

SSL_CTX *js_tls_ctx_create(void);
SSL     *js_tls_new(SSL_CTX *ctx, int fd, const char *hostname);
int      js_tls_handshake(SSL *ssl);
ssize_t  js_tls_read(SSL *ssl, void *buf, size_t len);
ssize_t  js_tls_write(SSL *ssl, const void *buf, size_t len);
void     js_tls_free(SSL *ssl);

#endif /* JS_TLS_H */
