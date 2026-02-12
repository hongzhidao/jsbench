#ifndef JS_WEB_H
#define JS_WEB_H

#define JS_MAX_URL_LEN      4096

/* ── Parsed URL ───────────────────────────────────────────────────────── */

typedef struct {
    char    scheme[8];       /* "http" or "https" */
    char    host[256];
    char    port_str[8];
    int     port;
    char    path[JS_MAX_URL_LEN];
    bool    is_tls;
} js_url_t;

/* ── HTTP request ────────────────────────────────────────────────────── */

typedef struct {
    js_url_t     url;
    char        *method;       /* "GET", "POST", etc. */
    char        *headers;      /* "Key: Value\r\n..." */
    char        *body;
    size_t       body_len;
} js_request_t;

int     js_parse_url(const char *url_str, js_url_t *out);
int     js_request_serialize(js_request_t *req, const char *host_override,
                              js_buf_t *out);
void    js_request_free(js_request_t *req);

void    js_headers_init(JSContext *ctx);
JSValue js_headers_from_http(JSContext *ctx, const js_http_response_t *parsed);

void    js_response_init(JSContext *ctx);
JSValue js_response_new(JSContext *ctx, int status, const char *status_text,
                        const char *body, size_t body_len,
                        const js_http_response_t *parsed);

void    js_fetch_init(JSContext *ctx);

#endif /* JS_WEB_H */
